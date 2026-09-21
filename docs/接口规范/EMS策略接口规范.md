# EMS 策略接口规范 v1.2

> EMS Strategy Interface Specification
>
> 文档编号：EMS-SIS-001 ｜ 版本：v1.2 ｜ 日期：2026-09-19 ｜ 状态：**试行**（v1.0 草案已签；v1.1 回应 6 项评审决议；v1.2 对齐 SSOT 策略清单）
>
> 适用项目：工商业储能 EMS 系统
>
> **作用**：本规范为所有 EMS 策略（9 个）提供统一的"接口契约"，确保任何新增/替换策略均符合相同的数据形状、调用时序、优先级语义与异常降级规则，从而可在不改动仲裁器（Arbiter）的前提下热插拔。
>
> **v1.2 摘要**：§4.10 的「9 策略优先级总览」原按 `code/`（Python 快照）口径撰写，与《工商业储能EMS调控策略设计方案》
> §三及 C++ 实现 `04/src/strategies_9.h` 不一致。本版以设计方案 §三为 SSOT 重新对齐，
> 并明确 `custom_script` / `soc_life_planner` 两个条目的归并去向（§4.10.2）。

---

## 0. 文档结构

| 章节 | 内容 |
|---|---|
| §1 | 目的与适用范围 |
| §2 | 术语与约定（功率符号、时间、优先级） |
| §3 | 4 个统一数据模型（Device / Realtime / Strategy / Command） |
| §4 | 9 个策略接口卡（输入 ↓ 计算 ↓ 输出 ↓ 限制） |
| §5 | 接口时序（采样 → 评估 → 仲裁 → 下发） |
| §6 | 异常处理与降级路径 |
| §7 | 扩展指南（如何新增第 10 个策略） |
| §8 | 评审决议（v1.1） |
| §9 | 附录：与现有源码映射（9.1 Python / **9.2 C++，v1.2 新增**） |

---

## 1. 目的与适用范围

### 1.1 编写目的

当下 9 个策略在 `code/` 快照中各自定义 `evaluate()`，但**没有面向产品/研发/测试的统一接口契约文档**，导致：

1. **跨语言不一致**：Python 实现与 C/C++ 控制器（`01-03` 重构后）形状不一致时无对照基准
2. **难以扩展**：新增策略需要看全部 9 个源码才能避免破坏优先级语义
3. **测试盲区**：边界场景的覆盖范围依赖个人记忆，无标准合同可对齐

**本规范提供**：

- 一份"策略接口合同"——任何策略必须声明输入、输出、计算限制、启动/停止/异常条件
- 一套"统一数据形状"——Device / Realtime / Strategy / Command 四个核心模型，覆盖策略生命周期所有 IO
- 一份"扩展模板"——第 10 个策略应如何填写此合同

### 1.2 适用范围

| 适用 | 不适用 |
|---|---|
| ✅ L0/L1/L2/L3 所有 EMS 策略实现 | ❌ 仲裁器（Arbiter）内部算法 |
| ✅ Python `code/` 快照与后续 C/C++ 控制器重写 | ❌ BMS / PCS 设备的私有协议 |
| ✅ 实时控制（100ms 量级）与计划优化（15min 量级） | ❌ SCADA / 上位机的人机界面 |
| ✅ 单元测试 / 集成测试用例设计 | ❌ 第三方能管平台对接（IIoT 网关） |

### 1.3 与其他文档的关系

```
docs/architecture.md        总设计方案
       │
       └─→ docs/接口规范/EMS策略接口规范.md   ← 本文档
                │
                ├─→ 01/ doc                  B 组 MILP 求解器规范
                ├─→ 02/ doc                  A 组安全约束规范
                └─→ 03/各控制器/ design.md   单控制器实现详细
```

---

## 2. 术语与约定

### 2.1 功率符号约定（全系统强制）

| 符号 | 含义 | 数值符号 |
|---|---|---|
| `P_bat` | 电池功率 | **放电为正（+），充电为负（−）** |
| `P_grid` | 关口并网点功率 | **进口为正（+），馈网为负（−）** |
| `P_load` | 本地负荷 | 正值（用电） |
| `P_pv` | 光伏出力 | 正值（发电） |
| 功率平衡 | `P_grid = P_load − P_pv − P_bat` | （任何时刻成立） |

> ❗**强约束**：所有策略的 `p_lower / p_desired / p_upper` 三个输出字段均遵循此约定；评审时若发现反号直接打回。

### 2.2 时间约定

| 概念 | 数值 |
|---|---|
| Unix 时间戳 | `timestamp` 字段为 `float` 秒（精度 1ms 即可） |
| 控制周期（Cycle） | 见各策略接口卡 §4 |
| 窗口 | `demand_window_s` 默认 900s（15min） |

### 2.3 优先级约定（L0-L3 严格不可逾越）

| 层级 | 取值 | 含义 | 仲裁语义 |
|---|---|---|---|
| **L0** | 0 | 硬安全保护（消防、BMS 禁止、变压器跳闸） | 任何 L1/L2/L3 **不得放宽**它收紧的边界 |
| **L1** | 1 | 运行安全与并网合规（BMS 降功率、变压器过载、防孤岛） | 同 L2/L3 不得放宽；可与 L0 同向收紧 |
| **L2** | 2 | 本地经济性（需量、防逆流、光伏平抑） | 同 L3 不得放宽；可被 L0/L1 收紧 |
| **L3** | 3 | 全局经济性（峰谷套利、需求响应、现货交易、SOC 规划） | 最低优先级，仅在不破坏上层边界时生效 |

> **仲裁规则**（任意时刻且每周期重新计算）：
> 1. 取所有 L0 输出的 `max(p_lower)` 与 `min(p_upper)`，作为绝对区间 `[PL_abs, PU_abs]`
> 2. 在剩余 L1/L2/L3 内同理逐层收敛
> 3. 最终 `p_command ∈ [最终下界, 最终上界]`，`p_desired` 用同优先级或更高优先级策略的加权平均

### 2.4 状态码约定

| 字段 | 取值 | 触发 |
|---|---|---|
| `active=True` | 策略本拍**有动作**（区间被收紧或产生了期望功率） | reason 非 idle |
| `active=False` | 策略本拍**待机**（区间等于父层基线，期望为 0） | reason 形如 `idle / within_xxx / normal` |
| `reason` | 自由文本（建议用枚举字符串） | 用于 UI 展示与日志聚合 |

### 2.5 仲裁规则（v1.1 新增，回应 Q2）

**逐层收敛区间**：

```
for layer in [L0, L1, L2, L3]:
    for r in StrategyResult(priority == layer):
        p_abs_lower = max(p_abs_lower, r.p_lower)
        p_abs_upper = min(p_abs_upper, r.p_upper)
```

**同层 `p_desired` 冲突仲裁**（v1.1 决策）：

1. **运行模式独占**：`ctx.mode` 只允许一个 L3 活跃 —— `TIMED` 用 `peak_valley`，`MPC` 用 `dispatch_optimizer`，`CUSTOM` 用 `custom_script`。其余两个 L3 策略本拍 `weight=0` 不参与收敛
2. **同模式内多策略**（如 MPC + SOC 规划器同时运行）：按 `weight`（0~1）等权加权平均
3. **跨层绝不平均**：L0/L1/L2 与 L3 之间**只取区间交集**，不参与 desired 收敛
4. **加权后的 `p_desired`** 必须落在已收敛的 `[p_abs_lower, p_abs_upper]` 内，否则按比例缩放到区间内并报警 `desired_clip`

> 示例：MPC 模式时，`dispatch_optimizer.weight=1.0` + `soc_life_planner.weight=1.0`（按规划结果修正）。假设 MPC 输出 `+50kW`，SOC 规划器收紧到 `[0, +40]`；加权后 desired = `+45kW`，但上层 L0/L1/L2 已收敛到上限 `+40kW`，则最终下发 `+40kW`，reason 标 `desired_clip`。

---

## 3. 统一数据模型

下面定义 4 个核心数据模型，是策略**全生命周期**的 IO 合同。所有字段类型与不可变属性强制生效。

### 3.1 Device Data Model（设备静态/慢变数据）

**用途**：描述 EMS 接入的物理设备及其电气参数与可下达的最大指令。变化频率低（小时级或更慢）。

```yaml
Device:
  id: string                 # 设备唯一标识 (e.g. "PCS-01", "BMS-01", "TR-01")
  type: enum                 # PCS | BMS | METER | PV_INVERTER | LOAD | TRANSFORMER
  rated_power_kw: float      # 设备额定功率（正数幅度）
  comm_timeout_ms: int       # 通信超时阈值（默认 1500）
  install_date: date         # 安装日期（用于 SOH 衰减）

DeviceRegistry:
  devices: List[Device]      # 当前 EMS 可见的全部设备
  find(id) -> Device
  alive(device_id, now_ms) -> bool   # 是否在 comm_timeout 内有心跳

DeviceLimits（运行时刷新）:
  pcs_rated_chg_kw: float    # PCS 额定充电幅度
  pcs_rated_dis_kw: float    # PCS 额定放电幅度
  bms_chg_limit_kw: float    # BMS 允许最大充电功率（动态降功率值）
  bms_dis_limit_kw: float    # BMS 允许最大放电功率
  bms_chg_forbidden: bool    # BMS 禁止充电
  bms_dis_forbidden: bool    # BMS 禁止放电
  transformer_capacity_kw: float   # 变压器物理容量（kVA 视在功率近似）
  d_target_kw: float         # 契约需量上限（kW）
  updated_at: timestamp      # 最近刷新时间
```

**派生函数**（强制实现）：
```
absolute_limits(DeviceLimits) -> (p_lower, p_upper)
    # 详见 §4 策略 9（bms_protection）；其余策略均以此为基线
```

### 3.2 Realtime Data Model（实时数据快照）

**用途**：每个控制周期（最快 100ms）由采集层刷新一次，作为所有 L0-L3 策略的统一输入。

```yaml
RealtimeSnapshot:
  timestamp: float                  # Unix 秒
  p_grid_kw: float                  # 关口并网点实时功率（>0 进口 / <0 馈网）
  p_pv_kw: float                    # 光伏出力（>0 发电）
  p_load_kw: float                  # 本地负荷（>0 用电）
  soc: float                        # 当前 SOC [0.0, 1.0]
  temperature_c: float              # 电池温度
  soh: float                        # 电池健康度 [0.0, 1.0]
  meters_alive: Dict[device_id, bool]   # 设备在线状态

DemandWindowState:
  window_s: float                   # 窗口长度（默认 900）
  t_elapsed_s: float                # 本窗口已用时间
  p_avg_past_kw: float              # 已过去时间的平均功率

PricingWindow:
  cur_tou_type: enum                # VALLEY | FLAT | PEAK | SHARP
  cur_tou_price: float              # 当前电价 (CNY/kWh)
  tou_prices: List[(start_s, end_s, type, price)]   # 24h 完整表
```

**幂等性要求**：周期内多次调用 `evaluate()` 同输入必须返回同输出（除非要显式声明策略有内部状态，如一阶滤波）。

### 3.3 Strategy Data Model（策略元数据与运行时状态）

**用途**：描述策略自身属性与持续状态，配合注册中心实现热插拔。

```yaml
StrategySpec:
  name: string                      # 全局唯一 (e.g. "bms_protection")
  priority: PriorityLevel           # L0 / L1 / L2 / L3
  description: string               # 一句话功能说明
  planned_week: int                 # 设计方案中的规划完成周
  version: string                   # 语义化版本（破坏接口变更必升 major）
  is_stateful: bool                 # 是否在 evaluate() 间保留内部状态
  compute_period_ms: int            # 本策略的评估周期（见 §4）

StrategyState（仅 is_stateful=True 时使用）:
  last_eval_at: timestamp
  last_result: StrategyResult
  internal: Dict[str, Any]          # 策略私有（如 Smoothing 的 _smooth_prev）
  persist_keys: List[str]           # v1.1 新增：列出需要跨重启持久化的 internal 子键
                                   # 例：anti_reverse 写 ["integral", "last_e", "stale_ticks"]
                                   #     pv_smoothing 写 ["_smooth_prev"]
                                   # 持久化目标：SQLite（详见 §6 异常处理）

StrategyResult（每拍输出）:
  source: string                    # 策略 name
  priority: PriorityLevel
  timestamp: float
  p_lower: float                    # 许可区间下界（放电为正→负值=充电方向）
  p_upper: float                    # 许可区间上界（正=最大放电）
  p_desired: float                  # 期望功率（不变量：p_lower ≤ p_desired ≤ p_upper）
  weight: float                     # 期望权重 [0,1]
  status: { active: bool, reason: str }

StrategyStatus:
  active: bool
  reason: str                       # 推荐枚举: bms_lock / demand_shaving / ...
```

**接口签名（强制）**：
```python
def evaluate(ctx: ControlContext) -> StrategyResult:
    """无状态访问；如需保留状态使用 self._internal 字典。"""
```

### 3.4 Command Data Model（指令与许可区间下发）

**用途**：仲裁器收敛所有策略输出后下发到 PCS / BMS 执行的最终命令。

```yaml
PermissionRange:                    # 许可区间（必下发）
  p_lower: float                   # 最小允许功率（充电为负）
  p_upper: float                   # 最大允许功率
  valid_until: timestamp           # 该区间有效期（周期数）

ControlCommand:                     # 期望指令（与 PermissionRange 打包下发）
  p_desired: float                 # 期望电池功率（仲裁器按权重收敛）
  weight_source: List[(strategy_name, weight)]   # 用于审计
  issued_at: timestamp
  cycle_id: int                    # 周期序号

DeviceAck:                          # 设备执行回执（用于健康监控）
  device_id: string
  accepted: bool
  actual_p_bat_kw: float           # 实际下发值
  reason: str                      # 拒绝时填原因
  acked_at: timestamp

FallbackCommand:                    # 异常降级指令
  level: enum                      # SAFE_STOP | HOLD_LAST | USE_L0_ONLY
  p_desired: float                 # 0 或维持上一拍
  reason: str
```

**下发时序**：每个控制周期下发 **一组** `{PermissionRange, ControlCommand}`，设备端必须按 `PermissionRange` 限幅后执行 `ControlCommand.p_desired`。

---

## 4. 9 个策略接口卡

> 每个策略一节，结构统一为 **基本属性 / 控制周期 / 启动·停止·异常条件 / 输入 ↓ 计算 ↓ 输出 ↓ 限制**。

---

### 4.1 策略 1：`peak_valley_arbitrage` — 峰谷套利（定时）

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `peak_valley_arbitrage` |
| 优先级 | **L3** |
| 规划完成周 | Week 6 |
| 是否状态化 | 否（pure） |
| 适用运行模式 | `TIMED` |

**控制周期**：**15 min**（对齐分时电价时段切换点 ±5s 内触发；也可 1 min 评估但不修改 desired）

**启动条件**：当前 `PricingWindow.cur_tou_type ∈ {PEAK, SHARP, VALLEY}`（平段不主动套利）

**停止条件**：
- 上层 L0/L1 区间不允许继续动作（`p_lower == p_upper == 0` 或 `p_desired` 落不进区间）
- 系统进入 `MAINTAIN` 或 `FAULT` 模式

**异常条件**：
- 电价数据缺失超过 1 小时 → 切到 `use_l3_default = 0`（保电）
- 电价数据异常（峰 < 谷）→ 报警并退出

**接口四元组**（输入 ↓ 计算 ↓ 输出 ↓ 限制）：

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.cur_tou_type`、`ctx.cur_tou_price`、`ctx.limits.*`（走 `absolute_limits`） |
| **计算** | `is_charging_window(VALLEY)` → `p_desired = p_lower`；`is_discharging_window(PEAK/SHARP)` → `p_desired = p_upper`；否则 `0` |
| **输出变量** | `StrategyResult { p_lower, p_upper, p_desired, status }`（不收紧区间，只贡献期望） |
| **限制** | `p_lower ≤ p_desired ≤ p_upper` 由绝对区间提供；只放电 / 只充电；平段保电 |

---

### 4.2 策略 2：`dispatch_optimizer` — 动态预测优化（MILP/MPC）

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `dispatch_optimizer` |
| 优先级 | **L3** |
| 规划完成周 | Week 6（求解器在 `01/battery_server`） |
| 是否状态化 | 否（每拍从外部 MPC 注入 `p_plan_kw`） |
| 适用运行模式 | `MPC` |

**控制周期**：**15 min**（滚动优化步长；每周期下发下一拍）

**启动条件**：模式 == `MPC` 且求解器在线（最近 1h 至少成功出 1 个解）

**停止条件**：
- 求解器失效 → 切 `TIMED` 模式 → 接管给峰谷套利
- 上层 L0/L1/L2 区间不允许 `ctx.p_plan_kw` 落入

**异常条件**：
- `p_plan_kw` 缺失超过 2 个连续周期 → `p_desired=0`，原因为 `plan_stale`
- 求解器返回 infeasible → `weight=0`（不贡献期望）+ 报警

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.p_plan_kw`（外部 MILP 注入）、`ctx.soc_ref`（目标 SOC）、`ctx.mode`、`ctx.limits.*` |
| **计算** | `p_desired = clamp(ctx.p_plan_kw, absolute_limits(ctx))` |
| **输出变量** | `StrategyResult { p_desired, weight=1.0, reason=f"mode=MPC, soc_ref={...}" }` |
| **限制** | `p_desired` 受 L0/L1/L2 区间限幅；MILP 求解失败时本策略 `weight=0` |

---

### 4.3 策略 3：`custom_script` — 用户自定义策略脚本

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `custom_script` |
| 优先级 | **L3** |
| 规划完成周 | Week 6 |
| 是否状态化 | 否（脚本内部自管） |
| 适用运行模式 | `CUSTOM` |

**控制周期**：**1 min**（脚本可读但不允许高频）—— 实际执行可由 Lua 引擎节流

**启动条件**：`ctx.mode == CUSTOM` 且已注入 `script` 函数

**停止条件**：
- `mode != CUSTOM` 或 `script is None`
- 脚本抛异常连续 3 次 → 自动禁用 + 报警
- 上层 L0/L1/L2 区间不允许脚本输出值

**异常条件**：
- 脚本返回 `None` → `value=0` 待机
- 脚本返回非数字 → 抛 `ValueError`，被包装为 `script_error`，`p_desired=0`
- 脚本超时（**>300ms**，v1.1 决策）→ `p_desired=0`，原因为 `script_timeout`，连续触发 3 次自动禁用该策略

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.*`（脚本可读全部字段）、`self.script(ctx)` 回调 |
| **计算** | `value = float(script(ctx))` → `p_desired = clamp(value, absolute_limits)` |
| **输出变量** | `StrategyResult { p_desired, reason="scripted" / "script_error" / "script_timeout" }` |
| **限制** | **沙箱**：禁止脚本访问 ctx 之外的全局状态；不允许无限循环（执行时长硬上限 **300ms** / 拍，v1.1 决策）；写操作仅能经回调 `set_p_desired()` |

---

### 4.4 策略 4：`demand_management` — 需量管理（窗口 + 死区）

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `demand_management` |
| 优先级 | **L2** |
| 规划完成周 | Week 5（对应 `03/demand_management_controller/`） |
| 是否状态化 | 否（核心计算 pure；迟滞状态在控制器内部） |
| 适用运行模式 | 全部 |

**控制周期**：**100 ms**（实时追踪窗口平均）；窗口重置瞬时切到 `idle`

**启动条件**：
- `current_window_avg > d_target − deadband(5 kW)` **且** `p_load − p_pv > 0`（有需要削峰的缺口）

**停止条件**：
- `current_window_avg < d_target − deadband − hysteresis(3 kW)` 且持续 `DEMAND_HOLD_SEC=2.0s`
- 进入新窗口时强制重新评估
- L0/L1 区间不允许继续放电

**异常条件**：
- 通信丢失 `meters_alive["METER"] == False` → `p_desired = 0`，原因为 `meter_loss`
- `demand_window_s == 0` → 默认值 `900s`，报警 `use_default_window`
- 采样点不在窗口内（跨界） → 窗口重置

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.d_target_kw`、`ctx.demand_window_s`、`ctx.t_elapsed_s`、`ctx.p_avg_past_kw`、`ctx.p_load_kw`、`ctx.p_pv_kw`、`ctx.limits.*` |
| **计算** | 剩余允许平均 `p_remain = (D·T − p_avg_past·t)/(T − t)`；需放电 `p_req = P_load − P_pv − p_remain`（仅当 `> DEMAND_DEADBAND_KW=5kW`）；`p_desired = clamp(p_req, 0, p_upper)` |
| **输出变量** | `StrategyResult { p_upper=p_req(收紧后), p_desired=p_req, status={active:..., reason:"demand_shaving"/"within_demand_target"} }` |
| **限制** | 只放电不充电；不主动拉低平均（不鼓励额外充电）；PID 闭环和爬坡率限制在实时控制器内体现 |

---

### 4.5 策略 5：`anti_reverse` — 防逆流控制

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `anti_reverse` |
| 优先级 | **L2** |
| 规划完成周 | Week 5（对应 `03/anti_reverse_controller/`） |
| 是否状态化 | 是（PI 积分项；持久化在 `_internal.integral`） |
| 适用运行模式 | 全部 |

**控制周期**：**100 ms**（实时控制；PI 周期 100ms）

**启动条件**：
- `P_grid ≤ p_grid_min_kw + 0`（已逆流或临界）
- 或上层需要主动预防：`P_grid ≤ p_grid_min_kw + 10 kW` 进入预紧

**停止条件**：
- `P_grid > p_grid_min_kw + hysteresis(5 kW)` 持续 2s
- L0/L1 区间不允许继续动作
- 使能关闭 `cfg.charge_enabled=false`

**异常条件**：
- `meters_alive["METER"] == False` → `output = 0`，safety 报警
- 连续 1 分钟积分饱和 → 清零（防止 wind-up 复活后产生尖峰）

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.p_grid_kw`、`ctx.p_grid_min_kw`、`ctx.p_load_kw`、`ctx.p_pv_kw`、`self._internal.integral`、`self._internal.last_e` |
| **计算** | 反向临界 `p_dis_limit = P_load − P_pv − p_grid_min`（放电方向）；PI 输出：`out = clamp(Kp·e + Ki·Ts·integral + Kff·P_pv, 0, p_upper)`；`e = (p_grid_min − P_grid)` |
| **输出变量** | `StrategyResult { p_upper=min(p_upper, max(p_dis_limit, 0)), p_desired=clamp(charging_req, p_lower, 0), status }` |
| **限制** | 不允许多次放电致馈网；安全侧误差（过充）锁死时强制 `output=0`；不主动充电除非逆流 |

---

### 4.6 策略 6：`pv_smoothing` — 光伏出力平抑

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `pv_smoothing` |
| 优先级 | **L2** |
| 规划完成周 | Week 5（对应 `03/pv_smoothing_controller/`） |
| 是否状态化 | 是（滤波状态 `_smooth_prev`） |
| 适用运行模式 | 全部 |

**控制周期**：**100 ms**（一阶低通 `Ts=0.1s`，时间常数 `τ=60s`，`α=τ/(τ+Ts)≈0.9983`）

**启动条件**：
- `|dP_pv/dt| > 平抑阈值`（默认 `> 5 kW/s`，与波动等级相关）
- `SMOOTH_SOC_MIN=0.20 ≤ SOC ≤ SMOOTH_SOC_MAX=0.80`

**停止条件**：
- `SOC ≥ SMOOTH_SOC_MAX` 或 `SOC ≤ SMOOTH_SOC_MIN` → 强制 `p_bat=0`
- `dP_pv/dt` 平滑后回到阈值以下
- L0/L1 区间不允许继续

**异常条件**：
- 通信丢失 `meters_alive["PV_INVERTER"]` → 不更新 raw，滤波保持衰减
- 第一拍无历史 → `smooth_prev = raw`（不放大波动）

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.p_pv_kw`、`ctx.soc`、`self._internal._smooth_prev` |
| **计算** | `α = τ/(τ+Ts)`；`smooth = α·smooth_prev + (1−α)·raw`；`p_bat = smooth − raw`；SOC 越界则 `p_bat = 0`；`p_desired = clamp(p_bat, p_lower, p_upper)` |
| **输出变量** | `StrategyResult { p_desired, status={active:\|p_bat\|>1e-6, reason:"smoothing"/"idle"} }` |
| **限制** | 不超过绝对区间；SOC 上下限外强制停；不主动改变 `p_upper` 区间（仅产 desired） |

---

### 4.7 策略 7：`transformer_management` — 变压器过载（两道防线 L1）

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `transformer_management` |
| 优先级 | **L1**（v1.1 升：原代码标 L2，评审确认其性质为"运行安全 / 物理设备保护"，与 BMS 降功率同层） |
| 规划完成周 | Week 4 |
| 是否状态化 | 否（反时限模型在控制器层，本策略仅作"瞬时过载"收紧） |
| 适用运行模式 | 全部 |

**控制周期**：**100 ms**（与 L0 同频）；反时限特性在控制器内 1s 周期评估

**启动条件**：等效负荷 `apparent_load = P_load − P_pv` **超过** `transformer_capacity_kw × 过载比`
- 一级（90%）：告警
- 二级（100%）：硬收紧 — 详见下表

**停止条件**：`apparent_load < capacity × 0.95` 持续 30s → 退回 `within_capacity`

**异常条件**：
- `transformer_capacity_kw == 0`（未配置）→ 策略空跑 `p_upper = p_upper`
- 容量配置小于 100 kVA → 报警 `transformer_unrealistic`

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.p_load_kw`、`ctx.p_pv_kw`、`ctx.limits.transformer_capacity_kw`、`ctx.limits.*` |
| **计算** | `apparent = P_load − P_pv`；`if apparent > cap: p_desired = clamp(apparent − cap, p_lower, 0)` `p_upper = 0`；`else: p_desired = 0` `p_upper = abs_max` |
| **输出变量** | `StrategyResult { p_upper, p_desired, status={active:overload, reason:"transformer_overload"/"within_capacity"} }` |
| **限制** | 过载时**绝对禁止放电**（加重冲击）；不解除 L0 区间；反时限电流曲线由实时控制器补充 |

---

### 4.8 策略 8：`soc_life_planner` — SOC 与寿命友好规划

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `soc_life_planner` |
| 优先级 | **L3**（v1.1 确认：仅做软约束，按规划结果修正不放宽上层） |
| 规划完成周 | Week 7 |
| 是否状态化 | 否 |
| 适用运行模式 | 全部 |

**控制周期（双轨，见 §5.2）**：
- **评估周期**：**1 min**（SOC 变化缓）
- **下发周期**：**100 ms**（与系统评估同步可输出，但仅在评估后修改内部决策）

**启动条件**：始终 active（软约束，不主动调整 desired，只收紧 `p_lower/p_upper`）

**停止条件**：
- 上层 L0/L1/L2 区间已被锁死（0 区间）
- 系统进入 `FAULT`

**异常条件**：
- `temperature_c` 读数超出 `[−20, 60]℃` → 报警 `sensor_outofrange`，本拍用上次值
- `soh == 0` → 视为 `1.0`（保守不收紧）

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.soc`、`ctx.temperature_c`、`ctx.soh`、`ctx.p_plan_kw`、`ctx.limits.*`、`SOC_*` 配置常量 |
| **计算** | SOH 折算 `p_lower_soh = −\|p_lower\| × soh_factor`；高温避冲：温度>38℃ 且 SOC>90% → 充电减半；深充深放：SOC≤10% 禁充，≥95% 禁放 |
| **输出变量** | `StrategyResult { p_lower=p_lower_soh, p_upper=p_upper_soh, p_desired=clamp(p_plan_kw, p_lower_soh, p_upper_soh), status }` |
| **限制（v1.1 强化）** | **L3 严格只收紧不放宽**：不得放大上层 L0/L1/L2 已经收敛的 `p_lower`（只允许往更负方向调）和 `p_upper`（只允许往更小方向调）；不影响 L0/L1/L2 的收紧；SOH 系数下限 0.1 避免区间完全锁死；本策略对仲裁器而言**只贡献区间修正**，desired 直接复用 `ctx.p_plan_kw`（避免与 `dispatch_optimizer` 重复加权） |

---

### 4.9 策略 9：`bms_protection` — BMS / 硬安全保护（L0）

**基本属性**

| 项 | 值 |
|---|---|
| 名称 | `bms_protection` |
| 优先级 | **L0** |
| 规划完成周 | Week 4 |
| 是否状态化 | 否 |
| 适用运行模式 | 全部（始终在线） |

**控制周期**：**100 ms**（与所有 L0/L1 同频）

**启动条件**：永远 active（每周期评估）

**停止条件**：无（只要 BMS 状态可读就持续输出区间）

**异常条件**：
- BMS 通信丢失 `meters_alive["BMS"] == False` 超过 1500 ms → `p_lower=0, p_upper=0`（封锁全部充放），`active=True, reason="bms_lock"`
- BMS 数据 5s 未更新 → 同上，最高级别 safety 锁定

**接口四元组**

| 阶段 | 内容 |
|---|---|
| **输入变量** | `ctx.limits.bms_chg_limit_kw`、`bms_dis_limit_kw`、`bms_chg_forbidden`、`bms_dis_forbidden`、`pcs_rated_chg/dis_kw` |
| **计算** | 见 `absolute_limits(ctx)`：取 BMS / PCS 的最小值；如 `forbidden=True` 该方向归零；双 forbidden 时 `[0, 0]` |
| **输出变量** | `StrategyResult { p_lower=absolute, p_upper=absolute, p_desired=0, status={active:forbidden_any, reason:"bms_lock"/"bms_ok"} }` |
| **限制** | **任何其他策略不得放宽此区间**；BMS 通信丢失必须按最保守 `[0, 0]` 处理；本策略永不降级到 L1/L2 |

---

### 4.10 9 策略优先级总览（v1.2 对齐 SSOT）

> **v1.2 变更**：v1.1 的本表按 `code/`（Python 快照）口径撰写，与《工商业储能EMS调控策略设计方案》
> §三「现有 9 个核心策略设计」以及 C++ 实现 `04/src/strategies_9.h` 的策略清单**不一致**
> （v1.1 多了 `custom_script` / `soc_life_planner`，少了「BMS 请求降功率」/「需求响应」）。
> 本版按上游 SSOT 重新对齐。详见 §4.10.2 的归并说明。

**SSOT 顺序（出现歧义时按此裁定）**：

```
《工商业储能EMS调控策略设计方案》§三（9 策略的书面定义）
        │  ① 以此为准
        ▼
本规范 §4.10（接口契约层：编号 / 层级 / 命名映射）
        │  ② 映射到各语言实现
        ▼
各语言实现命名：C++ `04/src/data_models.h :: strategy_id` ／ Python `code/*.py`
```

#### 4.10.1 九策略清单（与设计方案 §三 逐条对应）

| # | 设计方案 §三 | `strategy_id`（C++） | 实现类 | 优先级 | 运行模式 | 评估周期 | 状态化 |
|---|---|---|---|---|---|---|---|
| 1 | 策略一：BMS 禁止充放 | `S01_BMS_FORBID` | `BmsForbidStrategy` | **L0** 硬安全 | `kIdle` | 100 ms | 否 |
| 2 | 策略二：BMS 请求降功率 | `S02_BMS_DERATE` | `BmsDerateStrategy` | **L1** 运行安全 | `kIdle` | 100 ms | 否 |
| 3 | 策略三：变压器过载限功率 | `S03_TRANSFORMER_LIMIT` | `TransformerLimitStrategy` | **L1** 运行安全 | `kIdle` | 100 ms | 否 |
| 4 | 策略四：需量管理 | `S04_DEMAND_MGMT` | `DemandMgmtStrategy` | **L2** 本地经济 | `kCustom` | 100 ms | 否 |
| 5 | 策略五：防逆流 | `S05_ANTI_REVERSE` | `AntiReverseStrategy` | **L2** 本地经济 | `kIdle` | 100 ms | **是** |
| 6 | 策略六：光伏出力平抑 | `S06_PV_SMOOTHING` | `PvSmoothingStrategy` | **L2** 本地经济 | `kIdle` | 100 ms | **是** |
| 7 | 策略七：峰谷套利 | `S07_PEAK_VALLEY` | `PeakValleyStrategy` | **L3** 全局经济 | `kTimed` | 15 min | 否 |
| 8 | 策略八：动态预测优化 | `S08_FORECAST_OPT` | `ForecastOptStrategy` | **L3** 全局经济 | `kMPC` | 15 min | 否 |
| 9 | 策略九：需求响应 | `S09_DEMAND_RESPONSE` | `DemandResponseStrategy` | **L3** 全局经济 | `kCustom` | 1 min | 否 |

**层级分布**：L0 ×1 ／ L1 ×2 ／ L2 ×3 ／ L3 ×3。
运行模式的下发周期统一 100 ms（见 §5.1 双轨制）。

#### 4.10.2 v1.1 → v1.2 的两处归并

v1.1 表中有两个策略在 C++ 实现里**没有独立的策略类**，处理如下：

| v1.1 条目 | 去向 | 说明 |
|---|---|---|
| `custom_script`（L3，自定义扩展，脚本内自管） | → **`S09_DEMAND_RESPONSE`** | v1.1 里它是"通用自定义扩展槽"，与设计方案 §3.9「需求响应」是同一件事的两种叫法。C++ 侧把它具体化为需求响应策略（`RunMode::kCustom`，响应 `DrEvent{active, target_kw, end_ts}`）。**Q3 的 300 ms 超时硬上限仍适用**于任何 `kCustom` 策略 |
| `soc_life_planner`（L3，SOC / 寿命，1 min） | → **拆成两处** | ① **硬边界**（`soc_min` / `soc_max` 禁充放 + `soc_warn_low` / `soc_warn_high` 预警降额 + 滞环）下沉到 **`05/src/safety_engine.h` 的 SOC 约束**，属 **L0** 安全层，不再是 L3 经济策略；② **经济性寿命项**由 `01/` 的 MILP 滚动计划承载，经 `08/src/plan_loader.h` 装载进实时层。**Q4 的"只贡献区间修正、desired 复用 `ctx.p_plan_kw`"语义随之作废** —— 该策略已不再以独立 `StrategyResult` 形式参与仲裁 |

> **为什么把 SOC 硬边界从 L3 下沉到 L0**：v1.1 把它放在 L3，等于"经济性策略可以投票决定是否突破 SOC 限值"。
> 这是错的 —— SOC 禁充放是**物理安全边界**，必须由 L0 无条件收紧区间，任何经济性策略都不能越过。
> 这条改动正是设计方案 §十一「安全优先级最高，经济优先级最低」的直接落地，也是 `12/` 验收
> **A3-03**（SOC 上下限：绝对限 + 预警降额）所护栏的行为。

#### 4.10.3 与设计方案 §十一 的对应

| 设计方案 §十一 | 本规范层级 | 命中策略 |
|---|---|---|
| P0 | L0 `kL0_Safety` | S01 |
| P1 | L1 `kL1_SafeOp` | S02、S03 |
| P2 | L2 `kL2_LocalEcon` | S04、S05、S06 |
| P3 | L3 `kL3_GlobalEcon` | S07、S08、S09 |

层级语义（跨层只收紧不拉宽 ／ 同层取 `max(p_lower)` + `min(p_upper)` ／ 仅最低层做同层加权
／ `desired_clip` 到收敛区间）**以设计方案 §十一「层级语义」表为准**，本规范 §2.5 是实现说明。

---

## 5. 接口时序

```
┌──────────────────┐
│ 采集层 (RTU/IED) │  100ms~1s 周期
└────────┬─────────┘
         │ MeterReading[]
         ▼
┌──────────────────┐     ┌─────────────────────┐
│ RealtimeSnapshot │◄────│  DeviceRegistry     │
│  (见 §3.2)       │     │  静态设备表          │
└────────┬─────────┘     └─────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────┐
│           策略评估（按优先级并行 / 串行）     │
│   ┌──────────┐ ┌──────────┐ ┌──────────┐    │
│   │ L0: BMS  │ │ L1: Trans│ │ L2/L3 ...│    │
│   └────┬─────┘ └────┬─────┘ └────┬─────┘    │
│        └────────────┴────────────┘           │
│              StrategyResult[]                │
└────────────────┬─────────────────────────────┘
                 │
                 ▼
┌──────────────────────────────────┐
│   仲裁器（Arbiter）              │
│   ├─ 按 L0→L3 逐层收敛区间     │
│   ├─ 同层按 weight 加权 desired │
│   └─ 输出 {PermissionRange,     │
│           ControlCommand}       │
└────────────────┬─────────────────┘
                 │ 下发
                 ▼
┌──────────────────────────────────┐
│  PCS / BMS / 网关                │
│  └─ 按 PermissionRange 限幅执行  │
│  └─ 回执 DeviceAck              │
└──────────────────────────────────┘
```

**强约束**：
- 每个控制周期必须**完整跑一遍** L0→L3 全部策略；不允许跳过 L0/L1 而直接 L2/L3
- `RealtimeSnapshot` 在评估期间**冻结**（避免半拍不一致）
- 仲裁器收到任一 `StrategyResult` 校验失败（违反 `p_lower ≤ p_desired ≤ p_upper`）→ 抛出**整拍重试**

### 5.1 评估周期 vs 下发周期（v1.1 新增，回应 Q5）

**问题**：原规范各策略仅给出一个"控制周期"——但 L2/L3 策略评估频率与最终下发频率本质上是两个不同的概念。

**v1.1 决策：双轨制**

| 维度 | 评估周期 | 下发周期 |
|---|---|---|
| 含义 | `evaluate()` 多久算一次 | 最终 `ControlCommand` 多长时间下发一次 |
| 频率范围 | 100ms ~ 15min，按策略性质定 | **统一 100 ms**（与实时闭环对齐） |
| 谁驱动 | 策略内部计时器 / 事件触发 | 仲裁器统一时钟 |
| 允许的策略 | 所有 L0-L3 | 所有 L0-L3 |
| 关系 | 高频评估可选用低频结果 | 每拍合并最近一次评估的结果 |

**各策略双轨周期映射**：

| # | 策略 | 评估周期 | 下发周期 | 说明 |
|---|---|---|---|---|
| 9 | `bms_protection` | **100 ms** | 100 ms | 实时硬约束，评估=下发 |
| 7 | `transformer_management` | **100 ms** | 100 ms | 实时物理保护 |
| 5 | `anti_reverse` | **100 ms** | 100 ms | PI 控制器周期 |
| 6 | `pv_smoothing` | **100 ms** | 100 ms | 一阶低通 `Ts=0.1s` |
| 4 | `demand_management` | **100 ms** | 100 ms | 窗口滚动 + 实时追踪 |
| 8 | `soc_life_planner` | **1 min** | 100 ms | 高频下发但低频重新评估 |
| 3 | `custom_script` | **1 min** | 100 ms | 脚本低频执行 |
| 2 | `dispatch_optimizer` | **15 min** | 100 ms | MILP 求解周期 |
| 1 | `peak_valley_arbitrage` | **15 min** | 100 ms | 分时电价时段切换点附近 ±5s 触发 |

> **强约束**：任何评估周期 > 100 ms 的策略，必须在策略内部维护"最近一次评估结果"的缓存；下发时直接复用而非空跑。仲裁器每 100 ms 拉一次快照。

---

## 6. 异常处理与降级路径

| 异常场景 | 表现 | 降级动作 |
|---|---|---|
| **采集层超时** | `RealtimeSnapshot.timestamp` 停滞 >5s | 系统告警 → 控制切 `HOLD_LAST`，复用上一拍命令 |
| **BMS 通信丢失** | `meters_alive["BMS"]=False` | `bms_protection` 立即收紧到 `[0, 0]`（最保守） |
| **策略内部抛异常** | 某策略 `evaluate()` 抛错 | **该策略本拍视为不存在**，其他策略继续评估，异常上报 `strategy_exception` |
| **L3 求解器失败** | `dispatch_optimizer` 拿不到解 | `weight=0`，期望让位给 `peak_valley_arbitrage` |
| **自定义脚本超时** | `custom_script` **>300ms**（v1.1） | `value=0`，reason=`script_timeout`，连续 3 次自动禁用该策略 |
| **多策略 desired 冲突** | 同层期望加权后超出已收敛的 L0 区间 | 仲裁器按 `weight` 比例缩放，总和不超允许区间 |
| **所有策略 active=False** | 仲裁输出 `p_desired=0` | 设备端进入 idle，复用上一拍 `PermissionRange` |
| **状态化策略重启**（v1.1 新增） | `anti_reverse` / `pv_smoothing` 等 `is_stateful=True` 策略的 internal 在 EMS 重启时丢失 | 重启前自动 snapshot 到 `var/strategy_state.db`（SQLite），启动时按 `persist_keys` 回填 |

### 6.1 状态持久化机制（v1.1 新增，回应 Q6）

**存储目标**：本地 SQLite `var/strategy_state.db`，单表：

```sql
CREATE TABLE strategy_state (
  strategy_name TEXT PRIMARY KEY,
  updated_at    REAL NOT NULL,
  payload       BLOB NOT NULL      -- pickled (json / msgpack / protobuf) 的 internal 字典
);
```

**触发时机**：
- 每 10s 增量写一次（节流，避免高频 IO）
- 策略 `is_stateful=True` 且声明 `persist_keys` 时才生效
- EMS graceful shutdown（SIGTERM）前强制 flush 一次

**加载时机**：
- 策略实例化时（`__init__` 或 `bind()`），按 `persist_keys` 从 DB 读
- 读不到（首次运行 / DB 损坏）→ 视为零初值，告警 `strategy_state_first_run`

**接口签名（强制）**：

```python
class StatefulStrategy(BaseStrategy):
    persist_keys: list[str] = []     # 子类声明需要持久化的字段

    def snapshot(self) -> dict:
        """返回需要序列化的 internal 子字典。同策略同状态必须幂等。"""

    def restore(self, payload: dict) -> None:
        """从序列化字典恢复 internal 状态。"""
```

**强约束**：
- 不得把运行期数据（如 `last_result`）放入 `persist_keys`（这些是无状态重算的）
- 不得把 `PII` 或秘钥放入 `persist_keys`
- 写入失败不应阻塞策略运行（catch + log，继续）

---

## 7. 扩展指南——如何新增第 10 个策略

### 7.1 流程

1. **选定层级**：根据策略性质确定 L0/L1/L2/L3
2. **填写接口卡**：按 §4 的格式产出 5 段（基本属性 / 控制周期 / 启停异常 / 输入↓计算↓输出↓限制 / 输入输出变量）
3. **实现 `evaluate()`**：保持 pure 或显式声明 `is_stateful=True`
4. **挂接到注册表**：`registry.register(MyStrategy)`，无需改仲裁器
5. **测试矩阵**：
   - 单元测试：正常输入、边界、`p_lower ≤ p_desired ≤ p_upper` 不变量
   - 集成测试：在仲裁器中跑一遍，验证优先级收敛
   - 如策略 `is_stateful=True`：补充"持久化往返测试"（snapshot → 序列化 → restore 后与原状态等价）
6. **评审**：与本文档 §4 中已有 9 个对齐，签入 `docs/接口规范/新增策略-XX.md`

### 7.2 必须遵守的不变量

| 不变量 | 违反后果 |
|---|---|
| `p_lower ≤ p_desired ≤ p_upper` | 仲裁器整拍失败 |
| 同策略多次调用 `evaluate(ctx)` 同 ctx 应同输出（除 `is_stateful=True`） | 测试不稳、仲裁不确定 |
| 只收紧不放松上层（L3 不得放宽 L0/L1/L2 区间） | 安全风险 |
| 输出 `p_desired` 不得超过 `DeviceLimits` 给出绝对区间 | 物理设备故障 |
| `timestamp = ctx.timestamp`（保证可追溯） | 调试无从下手 |
| `persist_keys` 显式声明的状态字段必须可序列化（不允许函数 / 文件句柄等） | 重启后无法 restore |

| 不变量 | 违反后果 |
|---|---|
| `p_lower ≤ p_desired ≤ p_upper` | 仲裁器整拍失败 |
| 同策略多次调用 `evaluate(ctx)` 同 ctx 应同输出（除 `is_stateful=True`） | 测试不稳、仲裁不确定 |
| 只收紧不放松上层（L3 不得放宽 L0/L1/L2 区间） | 安全风险 |
| 输出 `p_desired` 不得超过 `DeviceLimits` 给出绝对区间 | 物理设备故障 |
| `timestamp = ctx.timestamp`（保证可追溯） | 调试无从下手 |

---

## 8. 评审决议（v1.1 更新）

> v1.0 草案的 6 项待确认问题已由作者给出明确决策并落地到正文（v1.1）。后续如需变更，走文末"变更日志"。

| Q# | 问题 | v1.1 决议 | 落地位置 |
|----|------|-----------|----------|
| **Q1** | `transformer_management` 优先级 | **改 L1**（从 L2 升）；与 BMS 降功率同层 | §4.7 基本属性、§4.10 总览表 |
| **Q2** | L3 同层多策略 desired 冲突 | **运行模式独占 + weight 等权 + desired_clip** | §2.5 仲裁规则（新增）、§5 时序 |
| **Q3** | `custom_script` 超时硬上限 | 统一 **300 ms / 拍**；连续 3 次超时自动禁用 | §4.3 限制 / 异常、§6 表格 |
| **Q4** | SOC 规划器层级 | 维持 **L3**，但强化语义：只贡献区间修正，desired 复用 `ctx.p_plan_kw`（**v1.2 作废**：硬边界下沉 L0，见 §4.10.2） | §4.8 限制段加粗强化 |
| **Q5** | 评估 vs 下发周期 | **双轨制**：评估按策略性质 100ms~15min；下发统一 100ms | §5.1 双轨（新增）、§4 各策略控制周期 |
| **Q6** | 状态持久化 | **SQLite** `var/strategy_state.db`；按 `persist_keys` 声明；10s 节流；SIGTERM 前 flush | §3.3 StrategyState 字段、§6.1 持久化机制（新增）、§7.2 不变量 |

---

## 9. 附录：与现有源码映射

### 9.1 Python（`code/` 只读快照）

| 规范条款 | 对应 Python 文件 | 行号 |
|---|---|---|
| §3 Device 模型 | `code/init_common.py`, `code/units.py` | — |
| §3 Realtime 模型 | `code/models.py` (`ControlContext`) | L94-120 |
| §3 StrategyResult | `code/models.py` | L29-71 |
| §3 DeviceLimits | `code/models.py` | L74-90 |
| §4.1 peak_valley | `code/peak_valley.py` | 全文件 |
| §4.2 dispatch_optimizer | `code/dispatch_optimizer.py` | 全文件 |
| §4.3 custom_script | `code/custom_script.py` | 全文件 |
| §4.4 demand_management | `code/demand_ctrl.py` + `code/strategy_config.py` (DEMAND_*) | — |
| §4.5 anti_reverse | `code/anti_reverse.py` + `code/strategy_config.py` (ANTI_REVERSE_*) | — |
| §4.6 pv_smoothing | `code/smoothing.py` + `code/strategy_config.py` (SMOOTH_*) | — |
| §4.7 transformer | `code/transformer.py` | — |
| §4.8 soc_life_planner | `code/soc_planner.py` + `SOC_*` 常量 | — |
| §4.9 bms_protection | `code/bms_protect.py` + `code/utils.py:absolute_limits()` | — |
| §2.3 优先级枚举 | `code/enums.py:PriorityLevel` | L12-35 |
| §4 配置常量 | `code/strategy_config.py` | 全文件 |
| 基类接口 | `code/base.py` | L16-45 |

> `code/` 是并行 Python 实现的**只读汇总视图**，不属于 C/C++ 仓库主体，重构时不要引用。

### 9.2 C++（`01/`~`08/` 交付主线，v1.2 新增）

| 规范条款 | 对应 C++ 文件 | 说明 |
|---|---|---|
| §2.3 优先级枚举 | `04/src/data_models.h :: enum class Priority` | `kL0_Safety=0` / `kL1_SafeOp=1` / `kL2_LocalEcon=2` / `kL3_GlobalEcon=3` |
| §3 数据模型 | `04/src/data_models.h` | `RealtimeSnapshot` / `DeviceLimits` / `StrategyResult` / `PowerCommand` |
| §4 策略基类 | `04/src/strategy_base.h` | `IStrategy`（`id/name/priority/mode/evaluate`）+ `RunMode{kTimed,kMPC,kCustom,kIdle}` |
| §4.1 ~ §4.9 九策略 | `04/src/strategies_9.h` | 9 个具体类，见 §4.10.1 清单 |
| §4.10 策略编号 | `04/src/data_models.h :: namespace strategy_id` | `S01_..S09_` 常量，与 §4.10.1 逐字一致 |
| §2.5 仲裁规则 | `04/src/strategy_arbiter.h` | L0→L3 区间收敛 + 同层加权 + `desired_clip` |
| §5 接口时序 | `07/src/realtime_loop.h :: EmsRuntime::step()` | 固定顺序的单拍闭环（**⓪ 限值刷新 →** ① 采集 → ② 状态 → … → ⑪ 反馈）。**⓪ 步是 2026-09-19 补的**：设备侧限值（`CFG.*` / `STA.BMS_*_FORBID` / 变压器容量）在运行期会变，适配器 `limits_are_live()==true` 时每拍刷新 |
| §3 Realtime 快照 | `07/src/rtdb/ems_point_table.h` | **32** 点表（MEAS 7 / CMD 3 / CFG 14 / STA **8**），点名为跨进程契约。STA 段末两点 `STA.BMS_CHG_FORBID` / `STA.BMS_DIS_FORBID` 是 **BMS 保护的安全输入**（2026-09-19 追加，必须追加在末尾以保证索引稳定） |
| §6 降级路径 | `06/src/state_machine.h :: EmsState` | INIT → SELF_CHECK → READY → NORMAL/DERATED → FAULT → EMERGENCY（锁存） |
| 硬安全边界 | `05/src/safety_engine.h` | 9 条约束折叠成单一 `(p_lower, p_upper)`；SOC 上下限在此（§4.10.2 ①） |
| §4.2 优化计划装载 | `08/src/plan_loader.h` + `01/build` 的 MILP JSON | `01/` 求解、`08/` 装载（§4.10.2 ②） |

**验收护栏**：`12/` 的 **A2** 维度逐个策略在"触发工况"下核对设计意图（9 项），
**A3** 维度核对 9 条安全约束（11 项）。规范条款与代码的一致性由此被机器守住。

---

## 文档审批

| 角色 | 签字 | 日期 |
|---|---|---|
| 产品负责人（本文档 Owner） | _待签_ | |
| 研发负责人（A 组 算法主线） | _待签_ | |
| 研发负责人（B 组 C++ 控制器） | _待签_ | |
| 测试负责人 | _待签_ | |
| 评审记录 | _待补_ | |

---

## 变更日志

### v1.2 (2026-09-19)

对齐 SSOT，修订策略清单口径：

- **§4.10 重写**：原表按 `code/`（Python 快照）口径，与设计方案 §三 + C++ `04/src/strategies_9.h`
  差两个条目。新版按设计方案 §三为 SSOT 列出 9 策略，并补 `strategy_id` / 实现类 / 运行模式列
- **新增 §4.10.1**：九策略清单（设计方案 §三名 ↔ C++ `strategy_id` ↔ 实现类 ↔ 层级 ↔ 运行模式）
- **新增 §4.10.2**：两处归并 —— `custom_script` → `S09_DEMAND_RESPONSE`；
  `soc_life_planner` 拆为「硬边界下沉到 `05/` L0 SOC 约束」+「经济性寿命项由 `01/` MILP 承载」
- **新增 §4.10.3**：与设计方案 §十一 P0~P3 的对应关系；层级语义以设计方案为准
- **§9 附录补 C++ 映射**：新增 `04/` ~ `08/` 的实现文件对照
- **作废条目**：Q4 中"soc_life_planner 维持 L3、desired 复用 `ctx.p_plan_kw`"的决议随 §4.10.2 一并作废
  （该策略已不再以独立 `StrategyResult` 参与仲裁）。Q1 / Q2 / Q3 / Q5 / Q6 全部继续有效

版本号：v1.1 → v1.2 ｜ 状态：试行（口径修订，非接口破坏性变更）

### v1.1 (2026-09-05)

回应 6 项评审决议，全部已落地：

- **Q1**：§4.7 `transformer_management` 优先级从 L2 升 L1；§4.10 总览表同步
- **Q2**：§2.5 新增"仲裁规则"小节；同层 desired 冲突采用"运行模式独占 + weight 等权 + desired_clip"
- **Q3**：§4.3 `custom_script` 超时硬上限从 500ms 收紧到 300ms；§6 表格同步；连续 3 次超时自动禁用
- **Q4**：§4.8 SOC 规划器维持 L3，强化"只收紧不放宽 + 只贡献区间修正"语义，desired 复用 `ctx.p_plan_kw`
  （**v1.2 作废**，见上）
- **Q5**：新增 §5.1 "评估 vs 下发双轨"，下发统一 100ms，评估按策略性质 100ms~15min
- **Q6**：§3.3 `StrategyState` 增加 `persist_keys` 字段；新增 §6.1 持久化机制（SQLite、10s 节流、SIGTERM flush）；§7.2 不变量补"必须可序列化"一条

版本号：v1.0 → v1.1 ｜ 状态：草案 → **试行**

### v1.0 (2026-09-05)

- 初版发布。基于 `code/` 9 个策略源文件 + `code/models.py` 数据模型 + `code/strategy_config.py` 默认参数
- 4 个数据模型（Device / Realtime / Strategy / Command）+ 9 个策略接口卡 + 接口时序 + 扩展指南

---

**版权与维护**：本规范由产品团队维护，任何接口级变更需走 CHANGES.md。
