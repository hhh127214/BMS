# 08/ —— 周期 8：优化调度与实时控制协同

> 本模块是 [`工商业储能EMS调控策略设计方案.md`](../工商业储能EMS调控策略设计方案.md) §7 **周期 8** 的落地实现，
> 建立「**优化层规划、实时层纠偏、安全层兜底**」的分层管控体系。

| | |
| --- | --- |
| 对应周期 | 周期 8：优化调度与实时控制协同 |
| 周期目标 | 「实现**优化层规划、实时层纠偏、安全层兜底**的分层管控体系。」 |
| 形态 | **纯头文件 C++17 库**（`src/plan_loader.h` + `src/dispatch_coordinator.h`）；`src/main.cpp` 为单场景演示 |
| 依赖 | `04/src`（`PlanTrackingStrategy` 依赖 `strategy_base.h`）；演示/端到端测试另需 `05/` `06/` `07/` |
| 单元测试 | `tests/test_dispatch_coordinator.cpp`，**T17~T20 / 444 断言，全过** |
| 演示 | `src/main.cpp` → 场景 D：24h 分层管控 |
| 关键常量 | 滚动重优化 `reopt_period_s = 900 s`；实时层纠偏上限 `total_correction_max_kw = ±100 kW`；计划 96 点 × 15 min |

---

## 1. 快速开始

```bat
cd 08
scripts\build.bat          :: 编 test_dispatch_coordinator.exe + coord_demo.exe
scripts\build_test.bat     :: 编 + 跑 T17~T20（应输出 PASS=444 FAIL=0 / ALL TESTS PASSED）
scripts\run_demo.bat       :: 跑场景 D（24h），输出 → build\demo_output.txt
```

生成 01/ 计划样例（可选，已随仓库提供）：

```bat
python scripts\gen_day_plan_sample.py     :: 写出 data\day_plan_sample.json
```

> **编译依赖**：`src` + `../04/src` + `../05/src` + `../06/src` + `../07/src`。
> T20 与场景 D 用 `07/` 的 `EmsRuntime` 做端到端，故需 `-I ../07/src`。

---

## 2. 文件导览

```
08/
├── src/plan_loader.h              ← 01/ MILP 计划 JSON 解析 + 贪心兜底规划器 + CSV 导出
├── src/dispatch_coordinator.h     ← 滚动重优化 + 3 项实时纠偏 + PlanTrackingStrategy
├── src/main.cpp                   ← 场景 D：24h 分层协同
├── tests/test_dispatch_coordinator.cpp  ← T17~T20
├── data/day_plan_sample.json      ← 01/ MILP 日间计划样例（96 点 × 15 min）
├── docs/README.md                 ← 本文件
├── docs/design.md                 ← 设计说明（分层职责 / 滚动优化 / 三项纠偏 / 兜底规划器 / 纠偏语义）
├── scripts/{build,build_test,run_demo}.bat
├── scripts/gen_day_plan_sample.py
└── build/                         ← 产物（git ignore；含 24h 时序 scenario8_24h.csv）
```

---

## 3. 分层职责

| 层 | 组件 | 周期 | 职责 |
| --- | --- | --- | --- |
| 预测分析层 | `ForecastSeries` | 15 min | 96 点负荷/光伏/电价时序，含电价分位与峰谷分类 |
| 优化层 | `IOptimizer` / `HeuristicOptimizer` / 01- MILP | 15 min 滚动 | 生成日间运行计划（充放电 + SOC 轨迹） |
| 实时层 | `DispatchCoordinator` | 100 ms | 以实测 SOC 对计划做**有界纠偏** |
| 安全层 | `SafetyEngine`（`05/`） | 100 ms | 9 类约束兜底，任何指令不得越界 |
| 执行层 | `PlantModel`（`07/`） | 100 ms | PCS 限幅执行，实测功率回灌 |

### 滚动时域优化

每 `reopt_period_s = 900 s` 以**当前实测 SOC** 为初值重算：

```cpp
size_t start_slot = static_cast<size_t>(now_ / fc_.step_s) % n;   // 按时段对齐
DayPlan np = opt_->solve(fc_, start_slot, rt.soc, soc_min, soc_max, cap, p_chg, p_dis);
```

始终规划**完整一个日周期**（`n` 个时段），预测序列按日周期回绕索引 —— 避免"接近日终时
计划长度收缩、充放电量塌到 0"。

### 实时层三项纠偏

`total_correction = clamp(soc_corr + dev_corr + energy_corr, ±total_correction_max_kw)`

| 项 | 作用 |
| --- | --- |
| SOC 反馈（P+I，带抗积分饱和） | 修正 SOC 相对计划轨迹的漂移 |
| 负荷偏差前馈 | 负荷突增/突降时提前补偿 |
| 窗口电量预算 | 本窗口已执行电量 vs 计划电量，避免"该充没充、该放没放" |

`coordinated_target_kw() = plan_target_kw() + total_correction_kw()` 作为 L3
`PlanTrackingStrategy` 的跟踪目标。

---

## 4. 场景 D 演示（`scripts\run_demo.bat`）

24h 闭环，96 次滚动重优化：

```
初始计划来源    : 01_plan_json（data/day_plan_sample.json）
滚动重优化      : 96 次（每 15 min 一次，24h 理论 96 次）
── ① 优化层（规划）──
末次计划充电/放电: 873.2 / 735.3 kWh（未来 24h 计划量）
── ② 实时层（纠偏）──
平均绝对纠偏量  : 22.10 kW（SOC 反馈 + 负荷前馈 + 窗口电量预算）
纠偏权限上限    : ±100 kW（超出部分由 L0/L1 安全层兜底）
── ③ 跟踪效果 ──
计划-实际偏差   : 均值 19.47 kW / 最大 250.00 kW
在计划上占比    : 74.8%（容差 ±10 kW）
── ④ 关口/需量 KPI ──
关口功率峰值    : 350.0 kW（契约需量 350 kW）
倒送功率        : 最大 0.0 kW，累计 0.0 s（0.0% 时间）
── ⑤ 安全层（兜底）──
安全裁剪次数    : 0（采样 8640 条，实际步数 864000）
状态门控次数    : 0；状态迁移 2 次
── ⑥ 能量账本与 SOC ──
实际充电 540.4 kWh / 实际放电 713.8 kWh / 末态 SOC 0.262
全过程 SOC ∈ [0.120, 0.844]（安全区间 [0.10, 0.90]）
── ⑦ 闭环指标 ──
samples=8640 mean_err=0.016kW rmse=0.897kW max_err=77.480kW steady_err=0.035kW
over_delivery=0.023% reversals=17(0.000/s) travel=1326.686kW(0.015/s)
max_reverse=0.000kW reverse_t=0.000s hold_last=0 safety_clip=0 state_gate=0
```

---

## 5. 贪心兜底优化器（`build_plan_greedy`）

当 01/ 的 MILP 计划不可用时降级使用。核心两步：

1. **动作分配**
   - 光伏余电时段（净负荷 `net < 0`）**必须充电吸收**（与电价无关，否则必然倒送）；
   - 最贵 1/4 时段放电、最便宜 1/4 时段充电（不覆盖已定的余电吸收时段）。
2. **光伏余电裕度预留**：从后往前累加"该时段之后还有多少余电必须吸收"，据此压低各时段的
   充电 SOC 上限。

```cpp
auto soc_cap_at = [&](size_t k) {
    const double reserve = std::min(soc_max - soc_min, surplus_ahead[k + 1] / usable_kwh);
    return std::max(soc_min, soc_max - reserve);
};
```

> **为什么必须预留**：早期实现把每个谷段都充到 `soc_max`，凌晨 3 点就满充 → 白天光伏大发时
> 没有裕度吸收 → 被迫倒送。效果：倒送从 83.5 kW / 113.6 s 降到 **0.0 kW / 0.0 s**，
> 且放电量不降反升。

---

## 6. 单元测试覆盖（T17~T20）

| 用例 | 覆盖点 |
| --- | --- |
| T17 | 计划 JSON 解析（01/ 格式）+ 时间采样 |
| T18 | 贪心兜底优化器（含光伏余电裕度预留） |
| T19 | 协调器纠偏有界 + 滚动重优化 |
| T20 | 24h 端到端分层协同（优化层规划 → 实时层纠偏 → 安全层兜底） |

---

## 7. 与其他模块的关系

- **与 `01/`**：`plan_loader.h` 直接解析 `01/docs/api.md` 定义的 MILP 响应 JSON；
  真实部署时把实测 SOC 回灌给 01/ 的 HTTP 服务重解即可（接口不变）。
- **与 `04/`**：`PlanTrackingStrategy` 作为 L3 的一条策略注册进 `04/StrategyManager`，
  在 MPC 模式下独占 L3（接口规范 §2.5 运行模式独占）。
- **与 `07/`**：`EmsRuntime::step()` 第 ⑤ 步调用本模块的 `coord_.update()`；
  第 ⑧ 步的 L2 实时纠偏语义（`merge_realtime_correction`）与 `03/integration` 的合并口径一致
  （防逆流 > 需量 > 平抑）。
- **与 `05/`**：纠偏幅度上限之外的部分由 L0/L1 安全层兜底 —— 优化层与实时层都**无法**突破安全边界。

---

## 8. 设计文档

分层职责、滚动时域优化、三项纠偏、兜底规划器、L2 纠偏语义（绝对目标 vs 增量）、
以及需量管理方向修正的完整设计说明见 [`docs/design.md`](./design.md)。
