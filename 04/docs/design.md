# 04/ 详细设计文档 — 策略管理层 + 仲裁器

> 配套文档：[`README.md`](./README.md)（功能/编译/运行）｜ [`接口规范`](../接口规范/EMS策略接口规范.md)（策略接口合同）
>
> 本文聚焦：算法推导、与接口规范对应、与其他模块的衔接、性能与正确性边界。

---

## 1. 问题域

### 1.1 多策略协同的难点

设计方案定义 9 个策略，分属 3 类：

| 类别 | 数量 | 协同难点 |
|---|---|---|
| 安全控制（§3.1-§3.3） | 3 | **不可逾越**：必须硬约束最高优先级 |
| 实时控制（§3.4-§3.6） | 3 | **互相不影响**：各自有独立 IO，但共享 PCS/BMS |
| 经济优化（§3.7-§3.9） | 3 | **互相冲突**：峰谷套利与需量管理可能同时要求相反功率 |

设计方案的 §3.7-§3.9 中的三个策略（峰谷套利、动态预测优化、需求响应）**均为"全时段"型经济优化**，理论上同一时刻只能有一个生效。
设计方案 §7 周期 4 的典型场景：

```
峰谷套利:     放电 +500 kW
需量管理:     放电 +400 kW
需求响应:     放电 +300 kW
BMS 动态上限:  250 kW
变压器上限:    200 kW
─────────────────────────────────
最终输出:     200 kW  ← L0/L1 区间 ∩ L2/L3 区间
```

### 1.2 接口规范的解决思路

[`EMS策略接口规范.md`](../接口规范/EMS策略接口规范.md) v1.1 §2.3-§2.5 提出三层防御：

1. **L0-L3 优先级分层**（§2.3）：硬安全 → 运行安全 → 本地经济 → 全局经济
2. **区间逐层收敛**（§2.5）：`[PL_abs, PU_abs] = ∩_L0 [p_lower, p_upper]` 逐层收紧
3. **desired 加权**（§2.5）：同层多策略的 `p_desired` 按 `weight` 加权平均
4. **跨层绝不平均**（§2.5）：L0/L1/L2 与 L3 之间**只取区间交集**，不参与 desired 加权

---

## 2. 仲裁器核心算法

### 2.1 输入/输出

```
输入:  vector<StrategyResult> results,  // 所有策略的输出（按 priority 升序）
       Timestamp now_s                  // 当前时间
输出:  PowerCommand cmd                  // 下发指令
```

`StrategyResult` 字段：
- `priority` ∈ {L0, L1, L2, L3}
- `p_lower`, `p_upper`（区间）
- `p_desired`（期望点）
- `weight` ∈ [0, 1]（同层加权权重）
- `active`（是否本拍产生动作）

### 2.2 区间逐层收敛

```cpp
abs_lower = -1e18;
abs_upper =  1e18;

for lvl in [0, 1, 2, 3]:                   // L0 → L3
    for r in results(priority == lvl):
        abs_lower = max(abs_lower, r.p_lower);
        abs_upper = min(abs_upper, r.p_upper);
```

**关键性质**：低优先级（L0）先处理，被高优先级策略收紧的区间会**覆盖**所有 L1/L2/L3 策略放宽的尝试。

> 例子：L0 BmsForbid 输出 `p_upper=0`（禁放），即使 L3 PeakValley 输出 `p_upper=+500`，收敛后 `abs_upper=0`。

### 2.3 同层 desired 加权（仅 L3）

```cpp
desired_sum = 0; weight_sum = 0;
for r in results(priority == L3 and r.active and r.weight > 0):
    desired_sum += r.p_desired * r.weight;
    weight_sum  += r.weight;
desired = desired_sum / weight_sum;     // weight_sum > 0
```

**L0/L1/L2 的 desired 不参与收敛**——这与直觉相反但符合"安全不参与经济"原则。
L0/L1 收紧的是**区间**，不是**期望**。期望仅由 L3 加权得到。

### 2.4 desired 必须落在区间内

```cpp
if desired < abs_lower:
    desired = abs_lower;  cmd.clamped = true; reason = "desired_below_lower";
if desired > abs_upper:
    desired = abs_upper;  cmd.clamped = true; reason = "desired_above_upper";
```

**典型场景验证**：
```
L0 BmsForbid:        p_lower=-∞, p_upper=+∞         (正常)
L1 BmsDerate:        p_lower=-100, p_upper=100       (PCS 100)
L1 TransformerLim:   p_lower=-100, p_upper=200       (PCS 200)
L2 DemandMgmt:       p_lower=-100, p_upper=200, desired=400
L2 AntiReverse:      p_lower=-100, p_upper=200, desired=0   (idle)
L2 PvSmoothing:      p_lower=-100, p_upper=200, desired=-50
L3 PeakValley:       p_lower=-100, p_upper=200, desired=500   ← 关键
L3 ForecastOpt:      p_lower=-100, p_upper=200, desired=45
L3 DemandResponse:   p_lower=-100, p_upper=200, desired=0   (idle)

区间收敛:
  L0: [-∞, +∞]
  L1: [-100, 200]    (BmsDerate 100, Transformer 200 → min=100 收敛到 100? no, 
                                          100 < 200 → abs_upper = 100)

wait，实际上是：BmsDerate p_upper = min(bms_dis=250, pcs_dis=100) = 100
                  TransformerLim p_upper = min(pcs_dis=100, ...) = 100
                  → abs_upper = 100

期望: (PeakValley 500*1 + Forecast 45*1) / 2 = 272.5
最终: clamp(272.5, -100, 100) = 100, clamped=true, reason=desired_clip(above)
```

### 2.5 死区 + 滞环

```cpp
if |desired| < deadband (e.g., 2.0):
    if hysteresis_keep (上次也是死区方向):
        desired = last_desired (保持上次方向)
    else:
        desired = 0
        hysteresis_cnt++;  if hysteresis_cnt >= switch_delay: 切换
```

**目的**：避免在 0 附近抖动。**`switch_delay=3`** 表示需要连续 3 拍都越过死区才切换方向。

### 2.6 interval_contradiction 兜底

```cpp
if abs_lower > abs_upper:    // L0 禁放同时 L0 禁充 → 区间 [0, 0] 才能闭合
    abs_lower = abs_upper = 0;
    cmd.clamped = true;
    cmd.reason = "interval_contradiction";
    return cmd;
```

---

## 3. 策略管理器设计

### 3.1 全生命周期

```
new Strategy()             // 构造
        ↓
register(mgr)              // mgr.register_strategy(sp)
        ↓
mgr.start(id)              // 参与 tick
        ↓
mgr.tick(rt, dev)          // 每拍调用
        ↓
mgr.set_param(id, k, v)    // 热更新参数（可任意时刻）
        ↓
mgr.stop(id)               // 不参与 tick
        ↓
mgr.unregister(id)         // 移除
        ↓
~Strategy()                // 析构
```

### 3.2 tick 输出顺序

`tick()` 按 priority 升序返回 `[L0, L0, ..., L1, L1, ..., L2, L2, ..., L3, L3, ...]`：

```cpp
std::vector<std::vector<StrategyPtr>> bucket(4);
for kv in registry:
    if kv.started and kv.enabled:
        bucket[priority].push_back(kv);

for lvl in [0, 1, 2, 3]:
    for sp in bucket[lvl]:
        results.push_back(sp->evaluate(rt, dev));
```

**好处**：仲裁器可直接按 `priority` 升序处理，无需额外排序。

### 3.3 状态监控

每个策略有一个 `StrategyStatus` 快照：

```cpp
struct StrategyStatus {
    std::string strategy_id;
    bool        enabled;
    bool        started;
    bool        last_active;
    Timestamp   last_eval_ts;
    std::string last_reason;
    double      last_p_lower;
    double      last_p_upper;
    double      last_p_desired;
};
```

`get_all_status()` 返回所有策略状态，用于 UI/告警/历史记录。

### 3.4 L3 独占模式

```cpp
void set_run_mode(RunMode m) {
    run_mode_ = m;
    for sp in registry:
        if sp.priority != L3: continue;
        bool match = (sp.mode() == m) || (m == kIdle);
        sp.set_param("__weight__", match ? 1.0 : 0.0);
}
```

**逻辑**：L3 三个策略（peak_valley / forecast / demand_response）分别属于 Timed / MPC / Custom 模式。
同一时刻只有匹配 `run_mode` 的策略 weight=1.0，其他 weight=0（不参与 desired 收敛）。

这是接口规范 §2.5 第 1 条的实现。

---

## 4. 9 策略参考实现

`strategies_9.h` 提供每个策略的简化算法版本，便于单元测试 + 演示。**实际工程** 应替换为 01/02/03 真实算法的薄包装器。

### 4.1 S01_BMS_FORBID（L0）

```
if bms_chg_forbidden:  p_lower = 0    (禁充 → P_bat < 0 禁)
if bms_dis_forbidden:  p_upper = 0    (禁放 → P_bat > 0 禁)
```

注：功率方向约定 P_bat>0 = 放电，P_bat<0 = 充电。

### 4.2 S02_BMS_DERATE（L1）

```
p_chg = min(bms_chg_limit, pcs_rated_chg)    [kW，正值]
p_dis = min(bms_dis_limit, pcs_rated_dis)    [kW，正值]
p_lower = -p_chg; p_upper = +p_dis;
```

### 4.3 S03_TRANSFORMER_LIMIT（L1）

```
tr_load_kw = |p_grid| + p_load * 0.1   (简化估算)
ratio = tr_load_kw / transformer_capacity

if ratio > 1.10:     p_upper = 0                (极端过载 → 禁放)
elif ratio > 0.95:   p_upper = min(pcs_dis, headroom_kw)   (轻度过载)
else:                p_upper = pcs_rated_dis
```

### 4.4 S04_DEMAND_MGMT（L2）

简化的预测 PI：基于窗口已用功率外推本窗口最终均值，期望放电补偿。

### 4.5 S05_ANTI_REVERSE（L2）

```
surplus = P_grid_min - P_grid   (kW)
if surplus > 0:                 (关口即将倒送)
    p_chg = Kp * surplus        (充电)
    p_desired = -p_chg
```

### 4.6 S06_PV_SMOOTHING（L2）

一阶滤波 + 偏差驱动：储能吸收光伏波动（充电增加 = P_bat 更负）。

### 4.7 S07_PEAK_VALLEY（L3, Timed 模式）

```
if tou == VALLEY:   p_desired = -P_charge
elif tou in PEAK/SHARP: p_desired = +P_discharge
else (FLAT):        p_desired = 0
```

SOC 保护：低 SOC 时禁止放电，高 SOC 时禁止充电。

### 4.8 S08_FORECAST_OPT（L3, MPC 模式）

简化版：负荷 > 需量目标时倾向放电，负荷 < 目标时倾向充电。

### 4.9 S09_DEMAND_RESPONSE（L3, Custom 模式）

外部事件触发：`event.active=true` 时按 `event.target_kw` 下发，到期后自动结束。

---

## 5. 测试覆盖

10 个测试用例（37 个断言）：

| T# | 名称 | 验证点 |
|---|---|---|
| T01 | Manager 注册/启停 | 注册/重复注册/启停/注销 |
| T02 | L0 覆盖 L1/L2/L3 | BMS 禁放 → 上界 0 |
| T03 | L1 覆盖 L2/L3 | BMS 降功率 → 上界收紧 |
| T04 | L3 同层加权 | PeakValley 独占权重 → desired=60 |
| T05 | 跨层绝不平均 | PeakValley+90 + BMS50 → 最终 ≤ 50 + clipped |
| T06 | desired_clip | desired=+500 → 压扁到区间上界 |
| T07 | 典型场景 | 设计方案周期4 原文场景：5策略 → 最终 ≤ 200 |
| T08 | 死区 + 滞环 | 0.5 kW < 死区 → 切 0；连续 2 拍才切 |
| T09 | L3 独占模式 | MPC 模式 → 仅 ForecastOpt weight=1 |
| T10 | tick 输出顺序 | L0 在前，L3 在后 |

---

## 6. 边界与已知限制

### 6.1 浮点精度

仲裁器所有区间运算用 `double`（IEEE 754），`±1e18` 作为 ±∞ 的近似。
两个边界条件：

- `p_lower > p_upper`：视为 `interval_contradiction`，强制下发 0
- `desired` 越界：用 `<` 和 `>` 而不是 `<=` 和 `>=`，避免边界相等时误标 `clamped`

### 6.2 策略注册失败

- `null` 策略 → `std::invalid_argument`
- 空 id → `std::invalid_argument`
- 重复 id → `std::invalid_argument`
- 操作未注册的 id → `std::invalid_argument`

**生产环境** 应捕获这些并转化为告警 + 持久化失败记录。

### 6.3 异常处理

策略 `evaluate()` 抛异常时由管理器兜底：

```cpp
try { r = sp->evaluate(rt, dev); }
catch (...) {
    r.active = false;
    r.reason = "exception_caught";
}
```

**重要**：不传播异常，不影响其他策略。

### 6.4 性能预算

设计要求：
- 实时控制周期 100 ms
- 9 策略 → 单次 `tick()` 预算 ≤ 10 ms（留 90% 给其他模块）

实测（MOCK 9 策略，单次 tick）：
- 9 个 `evaluate()` 调用 ≈ 0.1 ms
- 仲裁器 ≈ 0.01 ms
- 合计 < 1 ms

即使替换为 01/02/03 真实算法（每次 evaluate 1 ms），总预算也在 10 ms 以内。

---

## 7. 与接口规范的差异说明

### 7.1 完全一致

- 4 个数据模型字段名、语义、约定
- L0-L3 优先级分层
- 区间逐层收敛
- 同层 desired 加权
- 跨层绝不平均
- desired_clip 语义
- RunMode 独占模式

### 7.2 本目录扩展（不在接口规范中）

- **`StrategyStatus`**：状态监控结构体（接口规范未定义，监控用）
- **`mutex`**：并发安全（接口规范未强制；本目录提供）
- **死区 + 滞环**：详细参数（`set_deadband`, `set_switch_delay`）
- **`interval_contradiction` reason**：兜底异常时的字符串

### 7.3 本目录待对接

- **配置持久化**：接口规范提到需要持久化配置，本目录未实现（使用 in-memory + `set_param`）
- **历史曲线**：未实现（接口规范 §3.1 提到 `DeviceRegistry.find/alive`，本目录简化）
- **SOE（事件顺序记录）**：未实现（02/ 已实现局部 SOE）