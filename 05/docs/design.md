# 05/ 设计说明 —— 周期 5：统一安全约束引擎

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

## 1. 周期 5：安全约束引擎（`safety_engine.h`）

### 1.1 目标与定位

> 设计方案原文："实现所有策略指令无法突破设备及系统安全边界。"

`02/` 已有设备侧处理器（BMS 禁止充放 / 降功率 / 变压器过载）。本模块在其上做**系统级统一收敛**：
把 9 条约束全部折叠成一个 `(p_lower, p_upper)` 区间，**任何** L2/L3 经济策略的期望值都必须落在这个区间内。

### 1.2 9 类约束 → 区间语义

| 约束 | 层级 | 区间语义 |
| --- | --- | --- |
| `bms_forbid` | L0 | 禁充 → `p_lower = 0`；禁放 → `p_upper = 0`；通信丢失 → `[0, 0]` |
| `soc_limit` | L0 | 触及下限禁放 / 触及上限禁充 / 预警区折减 50% |
| `battery_temp` | L0 | ≥55℃ 禁充放（紧急）；≥45℃ 折减 50% |
| `bms_derate` | L1 | `min(BMS 上送限值, PCS 额定)` |
| `pcs_limit` | L1 | PCS 额定充放幅度 × 折减系数 |
| `transformer_limit` | L1 | 轻度过载按余量限放；极端过载禁放 |
| `grid_connect` | L1 | `P_grid ≥ g_min → p_upper ≤ base − g_min`；`P_grid ≤ g_max → p_lower ≥ base − g_max` |
| `grid_quality` | L1 | 频率/电压越限 → `[0, 0]`（并网合规） |
| `ramp_rate` | L1 | `|p_cmd − p_last| ≤ ramp_kw_per_s × dt` |

收敛方式与接口规范 §2.5 一致：逐层求交，`p_lower = max(所有下界)`、`p_upper = min(所有上界)`。

### 1.3 三个正交标志位（关键设计）

`ConstraintResult` 上用三个**互相正交**的布尔量描述一条约束，避免语义混淆：

```cpp
bool active = false;           // 本拍是否真正收紧了区间
bool counts_as_derate = true;  // 是否属于"设备降额"信号（→ 状态机进 DERATED）
bool binds_interval = true;    // 是否参与区间求交（变化率走 slew limiter，故为 false）
```

- **`active`** 用于 `binding` trace：只统计"本拍动作**且**自己的边界就是最终边界"的约束，从而回答"到底是谁在限"。
  > 早期版本把 8 条约束的"基线区间"也当成生效，导致 `derated` 恒为真、状态机一启动就进 DERATED。现按 `active` 过滤后修复。
- **`counts_as_derate`** 把"**硬禁闭**"与"**降额**"分开：
  - 硬禁闭（BMS 禁充放、SOC 触及极限、温度故障、并网静态限值、变化率）→ 不构成"设备降额"；
  - 降额（SOC/温度预警、BMS 降额、PCS 降额、变压器过载）→ 构成 DERATED。
- **`binds_interval`** 见 §1.4。

### 1.4 变化率为什么不是区间约束

若把变化率写成区间 `[p_last − d, p_last + d]` 参与求交，会出两类问题：

1. **冷启动锁死**：`p_last` 初值未知时区间退化，指令永远出不去；
2. **假区间矛盾**：光伏大发时并网要求 `P_bat ≤ −200`（必须满充），而变化率给出 `[−20, +20]`，求交得下界 `20 > 上界 −200` → 误判矛盾并把输出锁死为 0。

因此变化率以**后置 slew limiter** 形式实现（`SafetyEngine::apply()` 中相对上一拍**实际下发值**限速）：

```cpp
const double lo = last_p_cmd_ - d, hi = last_p_cmd_ + d;
if (cmd.p_bat_cmd_kw > hi)      cmd.p_bat_cmd_kw = hi;
else if (cmd.p_bat_cmd_kw < lo) cmd.p_bat_cmd_kw = lo;
```

安全性论证：区间是凸的，从安全点朝安全目标单调移动必然仍在安全区间内 —— 既保证不越界，又不会与其他约束产生假矛盾。

### 1.5 区间矛盾 ≠ 紧急停机

典型矛盾场景：**电池满充（禁充，下界 0）+ 光伏大发且不许倒送（上界 < 0）**。
此时唯一安全动作是输出 0（`strict_l0` 下区间收成 `[0, 0]`），语义属于"**系统受限运行**"。

```cpp
if (v.p_lower > v.p_upper) {
    v.contradiction = true;
    v.l0_active = true;
    v.l0_hard   = true;
    v.derated   = true;      // → 状态机进 DERATED（可自恢复）
    if (p_.strict_l0) { v.p_lower = 0.0; v.p_upper = 0.0; }
}
```

> **反例（已修正）**：早期实现把矛盾并入紧急条件，导致状态机进 **EMERGENCY 并锁存**。
> 在 24h 场景里表现为：12:40 满充遇光伏大发 → 锁死到人工复位 → 储能 **11 小时失去调节能力**，
> 计划跟踪偏差均值 90 kW、实际放电从 757 kWh 塌到 26 kWh。
> 矛盾属"无可行非零功率"，不是设备紧急 —— 必须走可自恢复的 DERATED。

### 1.6 并网边界的量测滤波（`grid_filter_alpha`）

`grid_connect` 的基准量 `base = P_load − P_pv` 直接来自电表，带量测噪声。
由于**安全层优先级最高**，`apply()` 必须把指令压进该边界 —— 结果是储能被迫跟随一个**抖动的安全边界**，
在并网点附近形成极限环（实测：24h 内方向反转 809 次、指令总行程 4548 kW）。

该抖动发生在安全层**之后**，下游任何死区/滞环都压不住（死区输出会被 `apply()` 重新拉回边界）。
**根治必须在量测侧**：

```cpp
double base_raw = rt.p_load_kw - rt.p_pv_kw;
if (!grid_base_init_) { grid_base_f_ = base_raw; grid_base_init_ = true; }
else if (p_.grid_filter_alpha < 1.0)
    grid_base_f_ += p_.grid_filter_alpha * (base_raw - grid_base_f_);
```

`grid_filter_alpha = 1.0` 表示不滤波（保持单测对原始约束的精确断言）；`0.05` ≈ 2 s 时间常数 @ `dt = 0.1s`。

---

## 关键不变量

| 不变量 | 保障位置 | 校验 |
| --- | --- | --- |
| 变化率不产生假区间矛盾 | `binds_interval = false` + 后置 slew limiter | `05/tests` T04 |
| 区间矛盾不升级为锁存紧急 | `SafetyEngine::evaluate()` 置 `derated` | `05/tests` T03 |
| 并网边界去抖不破坏静态约束语义 | `grid_filter_alpha`（默认 1.0 = 不滤波） | `05/tests` T06 |
| 9 类约束逐条语义稳定 | `check_*` 九个方法各司其职 | `05/tests` T01 |
| 收敛区间 = 各约束求交 | `p_lower = max(下界)` / `p_upper = min(上界)` | `05/tests` T02、T05 |


---

## 已知边界与后续工作

- **与 `02/` 的接口对齐**：当前 `safety_engine.h` 复用了 `02/` 的变压器负载估算口径（`|P_grid| + 0.1·P_load`），尚未直接链接 `02/` 的类；后续可把 `02/` 的 `SafetyConstraints` 作为一条 `ConstraintResult` 适配进来。
- **光伏限发**：当电池满充且光伏余电超过吸收能力时，物理上只能倒送或限发。本模块不具备光伏逆变器控制通道，只能检出区间矛盾并安全降级（输出 0）。根治手段是光伏限功率。
- **`grid_filter_alpha` 的整定**：默认 1.0（不滤波）以保持单测对原始约束的精确断言；工程部署建议 0.05~0.2，需按电表噪声水平与响应要求折中。

