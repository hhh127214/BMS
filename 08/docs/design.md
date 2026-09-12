# 08/ 设计说明 —— 周期 8：优化调度与实时控制协同

> 阅读前建议先看 [`../../docs/接口规范/EMS策略接口规范.md`](../../docs/接口规范/EMS策略接口规范.md) §2.5 / §5 / §6 与 [`../../工商业储能EMS调控策略设计方案.md`](../../工商业储能EMS调控策略设计方案.md) §7。

---

## 0. 全系统符号约定

| 符号 | 约定 |
| --- | --- |
| `P_bat` | **放电为正、充电为负** |
| `P_grid` | **取电为正、倒送（馈网）为负** |
| `P_load` | 恒 `> 0`（本地负荷） |
| `P_pv` | 恒 `> 0`（光伏发电） |
| 功率平衡 | `P_grid = P_load − P_pv − P_bat` |

由平衡式可把"并网约束"直接翻译成对 `P_bat` 的区间：

```
P_grid ≥ g_min  →  P_bat ≤ (P_load − P_pv) − g_min      （上界）
P_grid ≤ g_max  →  P_bat ≥ (P_load − P_pv) − g_max      （下界）
```

---

## 4. 周期 8：优化调度与实时控制协同（`plan_loader.h` + `dispatch_coordinator.h`）

> 设计方案原文："实现**优化层规划、实时层纠偏、安全层兜底**的分层管控体系。"

### 4.1 分层职责

| 层 | 组件 | 周期 | 职责 |
| --- | --- | --- | --- |
| 预测分析层 | `ForecastSeries` | 15 min | 96 点负荷/光伏/电价时序，含电价分位与峰谷分类 |
| 优化层 | `IOptimizer` / `HeuristicOptimizer` / 01- MILP | 15 min 滚动 | 生成日间运行计划（充放电 + SOC 轨迹） |
| 实时层 | `DispatchCoordinator` | 100 ms | 以实测 SOC 对计划做**有界纠偏** |
| 安全层 | `SafetyEngine` | 100 ms | 9 类约束兜底，任何指令不得越界 |
| 执行层 | `PlantModel` | 100 ms | PCS 限幅执行，实测功率回灌 |

### 4.2 滚动时域优化

每 `reopt_period_s = 900 s` 以**当前实测 SOC** 为初值重算：

```cpp
size_t start_slot = static_cast<size_t>(now_ / fc_.step_s) % n;   // 按时段对齐
DayPlan np = opt_->solve(fc_, start_slot, rt.soc, soc_min, soc_max, cap, p_chg, p_dis);
```

始终规划**完整一个日周期**（`m = n` 个时段），预测序列按日周期回绕索引 —— 避免"接近日终时计划长度收缩、充放电量塌到 0"的问题。

### 4.3 实时层三项纠偏

`total_correction = clamp(soc_corr + dev_corr + energy_corr, ±total_correction_max_kw)`

| 项 | 作用 |
| --- | --- |
| SOC 反馈（P+I，带抗积分饱和） | 修正 SOC 相对计划轨迹的漂移 |
| 负荷偏差前馈 | 负荷突增/突降时提前补偿 |
| 窗口电量预算 | 本窗口已执行电量 vs 计划电量，避免"该充没充、该放没放" |

`coordinated_target_kw() = plan_target_kw() + total_correction_kw()` 作为 L3 `PlanTrackingStrategy` 的跟踪目标。

### 4.4 贪心兜底优化器（`build_plan_greedy`）

当 01/ 的 MILP 计划不可用时降级使用。核心两步：

1. **动作分配**
   - 光伏余电时段（净负荷 `net < 0`）**必须充电吸收**（与电价无关，否则必然倒送）；
   - 最贵 1/4 时段放电、最便宜 1/4 时段充电（不覆盖已定的余电吸收时段）。
2. **光伏余电裕度预留**：从后往前累加"该时段之后还有多少余电必须吸收"，据此压低各时段的充电 SOC 上限。

```cpp
auto soc_cap_at = [&](size_t k) {
    const double reserve = std::min(soc_max - soc_min, surplus_ahead[k + 1] / usable_kwh);
    return std::max(soc_min, soc_max - reserve);
};
```

> **为什么必须预留**：早期实现把每个谷段都充到 `soc_max`，凌晨 3 点就满充 → 白天光伏大发时没有裕度吸收 → 被迫倒送（安全层只能检出区间矛盾，无法物理避免）。
>
> 而且预留**在经济上更优**：余电是免费电量，比谷段 0.30 元/kWh 买电便宜，应优先吸收。
> 效果：倒送从 83.5 kW / 113.6 s 降到 **0.0 kW / 0.0 s**，且放电量不降反升。

### 4.5 L2 实时纠偏的语义（`merge_realtime_correction`）

三个 L2 控制器的 `p_desired` **语义不同**，不能一律当成增量相加：

| 控制器 | 语义 | 处理 |
| --- | --- | --- |
| 防逆流 | 绝对目标（我要求 `P_bat` **至少充到** `p_rev`） | 转成"只在 `p_cmd > p_rev` 时补 `p_rev − p_cmd`" |
| 需量管理 | 绝对目标（我要求 `P_bat` **至少放到** `p_dem`） | 转成"只在 `p_cmd < p_dem` 时补 `p_dem − p_cmd`" |
| 光伏平抑 | 增量（相对当前出力再吸收多少） | 直接叠加 |

> **反例（已修正）**：早期实现把绝对目标直接加到 L3 优化指令上，等于把"目标"当成"增量"重复计入 ——
> 有日间计划时会出现指令被顶到边界、计划跟踪失效（实测计划-实际偏差均值 90 kW）。
> 转换后在 `p_cmd ≈ 0`（无计划 / 纯实时控制）时与原实现完全等价，故不改变周期 7 的行为。

### 4.6 需量管理的方向（`04/src/strategies_9.h` v1.2）

需量管理的目标是"**不突破**契约需量"，而不是"把窗口均值填到目标"：

```cpp
double err = p_pred_avg - D_target;              // >0 = 将超限，需要放电
double p_des = (err > 0.0) ? Kp * err : 0.0;     // 低于目标时保持待机
```

> **反例（已修正）**：原实现是 `err = D_target - p_pred_avg`，只要预测均值**低于**目标就放电，
> 导致轻载时段无意义放电、SOC 被抽干（24h 场景下实际放电塌到 26 kWh）。

---

## 关键不变量

| 不变量 | 保障位置 | 校验 |
| --- | --- | --- |
| 实时层纠偏幅度有界 | `total_correction_max_kw` / `l2_correction_max_kw` | `08/tests` T19 |
| 24h 内不突破契约需量、不越 SOC 界 | 安全层 + 计划裕度预留 | `08/tests` T20 |
| 优化层始终规划完整日周期 | `DispatchCoordinator::reopt()` 按日周期回绕 | `08/tests` T19 |
| 光伏余电时段必被安排吸收 | `build_plan_greedy` 动作分配 | `08/tests` T18 |
| 跨层绝不平均，只有 L3 参与 desired 加权 | `04/strategy_arbiter.h` | `04/tests` T02–T05 |


---

## 已知边界与后续工作

- **01/ MILP 的进程内替代**：滚动重优化在进程内用启发式近似；真实部署应把实测 SOC 回灌给 01/ 的 HTTP 服务重解（接口不变）。演示输出已显式标注该降级（`初始计划来源` / `末次计划来源`）。
- **`PlanTrackingStrategy` 落在本模块**：它作为 L3 的一条策略注册进 `04/StrategyManager`，目标是"跟踪 `coordinated_target`"，属周期 8 的协同职责。
- **预测精度**：`ForecastSeries` 目前由外部注入；接入真实气象/负荷预测时需评估预测误差对滚动重优化的影响（当前演示为完美预测）。

