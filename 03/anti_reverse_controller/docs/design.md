## 防逆流控制器详细设计

### 1. 控制目标

- **核心目标**：保证并网点（关口）功率 `P_grid` 始终不低于设定的下限 `P_grid_min`（通常为 0kW 或 - 5kW），即不允许有功功率倒送电网。
- **次要目标**：在满足防逆流的前提下，尽量不影响储能系统的其他功能（如峰谷套利充电），避免不必要的功率限制。

### 2. 系统模型与变量定义

#### 2.1 功率平衡方程

在关口处，功率满足：

```
P_grid = P_load - P_pv - P_bat
```

其中：

- `P_grid`：电网向用户供给的功率（kW），正为买电，负为倒送。
- `P_load`：负载消耗功率（kW），始终为正。
- `P_pv`：光伏发电功率（kW），始终为正。
- `P_bat`：储能放电功率（kW），正为放电，负为充电。

**防逆流的本质**：当光伏出力 `P_pv` 大于负载 `P_load` 时，若储能不吸收多余功率（即 `P_bat` 不够负），`P_grid` 会变为负值，产生逆流。所以防逆流控制器需要动态调整储能的充电功率 `P_bat`，使得：

```
P_bat ≤ P_load - P_pv - P_grid_min
```

由于 `P_load` 和 `P_pv` 是实时变化的，控制器需要实时测量 `P_grid` 并调节 `P_bat`。

#### 2.2 控制变量与测量量

表格

| 变量 | 来源 | 单位 | 方向约定 |
| --- | --- | --- | --- |
| `P_grid` | 关口电表 | kW | 正 = 电网向用户供电，负 = 向电网倒送 |
| `P_pv` | 光伏逆变器 | kW | 正 = 光伏发电 |
| `P_bat` | 储能 PCS | kW | 正 = 储能放电，负 = 储能充电 |
| `P_load` | 计算得到 | kW | 正 = 负载消耗功率 |
| `P_grid_min` | 用户设定 | kW | 允许的最小并网功率，如 0、-5 |
| `P_chg_max` | BMS | kW | 储能最大允许充电功率（正值） |

### 3. 控制算法设计

#### 3.1 为什么选择 PI 控制器 + 前馈

防逆流控制对响应速度要求极高（<50ms），且需要消除稳态误差。PI 控制器结构简单、参数容易整定，能够满足要求。同时，加入**光伏功率变化率前馈**可以提前动作，减少超调。

#### 3.2 控制器结构

```
             ┌─────────────┐
P_grid_min ──> 误差计算     │
             │  e = P_grid_min - P_grid
             └──────┬──────┘
                    ▼
             ┌─────────────┐
             │    PI调节   │──> P_pi
             └──────┬──────┘
                    │
P_pv(k),P_pv(k‑1)──►光伏前馈──> P_ff
                    │
                    ▼
             P_charge_req = P_pi + P_ff
                    │
             ┌──────▼──────┐
             │ 限幅与死区  │──> 输出充电功率指令（正值）
             └─────────────┘
```

最终输出为充电功率需求 `P_charge_req`（正值，表示需要储能充电的功率）。在合并模块中会转换为负的 `P_bat` 指令。

#### 3.3 离散化 PI 算法

控制周期 `Ts = 0.1s`（100ms）。

```
e(k) = P_grid_min - P_grid(k)
P_pi(k) = Kp * e(k) + Ki * Ts * sum_{i=0}^{k} e(i)
```

为了防止积分饱和，采用**积分限幅 / 遇限削弱积分**策略：

- 当 `P_charge_req` 达到上限 `P_chg_max` 且 `e(k) > 0`（还需要更多充电），停止积分累加。
- 当 `P_charge_req` 达到下限 0 且 `e(k) < 0`（充电已经过多），停止积分累加。
- 除 “遇限削弱积分” 抗饱和逻辑外，增加积分项全局硬限幅，防止长时间大误差下浮点数无限累积溢出；
- PI 为位置式实现，`integral = sum(e(i))`，I 项计算：\(P_{int}=K_i \cdot T_s \cdot \mathrm{integral}\)。
- **陈旧积分清零保护（2026-08 修订）**：遇限削弱积分只“停止累加”不清零，当系统长时间大盈余充电（积分已积累到数万）后盈余骤降，陈旧积分会导致安全侧过充与振荡。为此增加一条清零规则：当 `e(k) < -deadband_exit`（电网供给充裕的安全侧）且输出仍大于 0 连续 3 拍，判定为积分陈旧导致过充，积分清零。同时规定：输出钳位在 0 时**不**清零积分（保留充电跟随能力，避免波动场景跟踪退化）。


#### 3.4 光伏前馈项

光伏功率的突然增加是逆流的主要诱因，引入变化率前馈提前干预：

```
P_ff(k) = Kff * (P_pv(k) - P_pv(k‑1)) / Ts
```

`Kff`：前馈增益，单位 s；初始建议 0.5~2.0。

#### 3.5 死区与迟滞

> 原文档只有单死区`deadband`；代码实现**带迟滞的双阈值死区**，防止边界频繁抖动。
> 
> 
> 为避免光伏功率微小波动导致储能频繁启停动作，设置误差死区 + 迟滞：

1. 进入死区条件：\(|e(k)| < deadband\_enter\)，进入死区，维持上一拍输出，**不更新积分项**；
2. 退出死区条件：\(|e(k)| > deadband\_exit\)，才退出死区恢复调节；
3. 要求：\(deadband\_exit > deadband\_enter\)，形成施密特迟滞；
4. 死区内持续跟踪`P_pv`历史值，避免退出死区瞬间光伏前馈产生阶跃尖峰。

> 
> 当充电使能`charge_enabled=false`被置位时：控制器输出强制置 0，积分清零，标记首次更新标志；当充电重新使能，第一拍会重新初始化光伏历史值，避免前馈出现冲击。
### 4. 参数整定指南

表格

| 参数 | 符号 | 建议初始值 | 整定方法 |
| --- | --- | --- | --- |
| 比例系数 | `Kp` | 0.5 ~ 1.0 | 增大加快响应；过大会引发振荡 |
| 积分系数 | `Ki` | 0.05 ~ 0.1 | 消除稳态误差；过大会加大超调 |
| 前馈增益 | `Kff` | 0.5 ~ 2.0 | 光伏波动剧烈场景可取偏大 |
| 死区进入阈值 | `deadband_enter` | 2 ~ 3 kW | 小于退出阈值 |
| 死区退出阈值 | `deadband_exit` | 4 ~ 5 kW | 迟滞，抑制边界抖动 |
| 积分上限 | `integral_max` | 按公式整定 | 须满足 `integral_max ≥ 最大持续充电需求/(Ki·Ts)`，防止积分漂移溢出 |
| 积分下限 | `integral_min` | -integral_max | 与上限对称即可 |
| 控制周期 | `Ts` | 0.1 s | 固定 100ms |

> 
> 说明：
> 
> 
> 1. 原单参数`deadband`拆分为`deadband_enter`、`deadband_exit`双阈值；
> 2. 新增`integral_max / integral_min`积分硬限幅参数，做数值保护；
> 3. PI 公式为位置式：\(u_{pi}=K_p e + K_i T_s \sum e(i)\)，**不可直接套用网上增量 PID 参数**。

**整定步骤**（仿真 / 现场）：

1. 将 `Ki=0`、`Kff=0`，只调 `Kp`；模拟光伏阶跃突增，调至响应快、超调可接受。
2. 打开 `Ki`，消除稳态残差，让关口功率稳定在 `P_grid_min`附近。
3. 打开 `Kff`，测试光伏快速扰动，微调至响应快且无振荡。

**积分上下限整定约束（2026-08 修订）**：位置式 PI 的积分项最大输出为 `Ki·Ts·integral_max`。若该值小于系统最大稳态充电需求，控制器将无法把 `P_grid` 收敛到 `P_grid_min`，稳态残留倒送。示例：7.1 场景最大盈余 100~150kW，取 Ki=0.1、Ts=0.1 → 所需积分 = 150/(0.1×0.1)=15000，建议 `integral_max=60000`（留足裕量）。文档早期默认 5000 仅适用于小功率系统，已废弃。

**关于响应时间的说明（2026-08 修订）**：`Ts=0.1s` + 迟滞死区（进入/退出阈值 2/4kW）下，阶跃扰动不可避免存在 1~2 拍（100~200ms）的固有瞬时偏差（扰动发生瞬间误差恰处于死区内被冻结）。比例项/前馈在退出死区当拍即动作，但完整收敛时间由积分项决定：位置式 PI 中积分项每拍仅增加 `Ki·Ts·e`，对 100kW 级阶跃，`Ki=0.05~0.1` 时收敛时间常数为 14~29s，进入死区约需 50~70s。因此文档早期"响应<200ms、超调<5kW"的表述应理解为**渐变（斜坡）扰动下比例/前馈的初次动作时间**，阶跃扰动的完整收敛请以仿真实测指标为准。

**Kp 稳定性提示（2026-08 修订）**：在 Ts=0.1s 且关口功率一拍采样延迟的离散系统中，`Kp` 接近 1.0 时闭环处于临界稳定（闭环特征根模长≈1），可能激发两拍交替振荡；建议 `Kp ≤ 0.5~0.75`，仿真默认取 0.5。

### 5. 伪代码实现

class AntiReverseController:
    def __init__(self, config):
        self.Kp = config['Kp']
        self.Ki = config['Ki']
        self.Kff = config['Kff']
        self.Ts = config['Ts']
        self.deadband_enter = config['deadband_enter']
        self.deadband_exit = config['deadband_exit']
        self.P_grid_min = config['P_grid_min']
        self.integral_max = config['integral_max']
        self.integral_min = config['integral_min']

        self.integral = 0.0
        self.prev_P_pv = 0.0
        self.last_output = 0.0
        self.in_deadband = False
        self.first_update = True
        self.safe_overcharge_cnt = 0   # 安全侧持续过充拍数（陈旧积分清零用）

    def update(self, P_grid, P_pv, P_chg_max, charge_enabled):
        if P_chg_max < 0.0:
            P_chg_max = 0.0

        if not charge_enabled:
            self.integral = 0.0
            self.last_output = 0.0
            self.in_deadband = False
            self.prev_P_pv = P_pv
            self.first_update = True   # 恢复充电时重新初始化光伏历史
            return 0.0

        if self.first_update:
            self.prev_P_pv = P_pv
            self.first_update = False

        error = self.P_grid_min - P_grid

        # 带迟滞死区
        if not self.in_deadband:
            if abs(error) < self.deadband_enter:
                self.in_deadband = True
                self.prev_P_pv = P_pv
                return self.last_output
        else:
            if abs(error) > self.deadband_exit:
                self.in_deadband = False
            else:
                self.prev_P_pv = P_pv
                return self.last_output

        P_prop = self.Kp * error
        P_int = self.Ki * self.Ts * self.integral

        dP_pv = (P_pv - self.prev_P_pv)/self.Ts
        P_ff = self.Kff * dP_pv
        self.prev_P_pv = P_pv

        P_unclamped = P_prop + P_int + P_ff
        output = max(0.0, min(P_chg_max, P_unclamped))

        saturated_high = (output >= P_chg_max) and (error > 0)
        saturated_low  = (output <= 0.0) and (error < 0)
        if not saturated_high and not saturated_low:
            self.integral += error

        # 陈旧积分清零保护：安全侧(e<-deadband_exit)持续过充3拍 → 积分清零
        if error < -self.deadband_exit and output > 0:
            self.safe_overcharge_cnt += 1
            if self.safe_overcharge_cnt >= 3:
                self.integral = 0.0
                self.safe_overcharge_cnt = 0
        else:
            self.safe_overcharge_cnt = 0

        # 积分硬限幅
        self.integral = max(self.integral_min, min(self.integral_max, self.integral))

        self.last_output = output
        return output

    def reset(self):
        self.integral = 0.0
        self.prev_P_pv = 0.0
        self.last_output = 0.0
        self.in_deadband = False
        self.first_update = True
        self.safe_overcharge_cnt = 0


### 6. 异常处理与保护

- **关口电表通信中断**：立即将输出置 0，并触发告警。禁止继续调节，防止误动作。
- **BMS 禁止充电**（来自 A 组）：`charge_enabled = false` 时，强制输出为 0，并在状态字中标记 “防逆流被禁止”。
- **SOC 达到上限**：如果电池已满，无法继续充电，此时防逆流控制器应输出 0，并通知光伏逆变器进行限功率（或由上层策略处理）。
- **PCS 未响应**：若连续 3 个周期检测到实际充电功率与指令偏差 > 20%，应降低输出并告警。
- **充电使能恢复保护**：当`charge_enabled`由 false 切为 true，控制器首拍自动初始化光伏历史值，避免光伏中间发生阶跃时前馈产生冲击尖峰。

> **职责边界说明（2026-08 修订）**：上述保护（关口电表通信中断、PCS 未响应、SOC 上限）属于上层监控/合并模块职责，控制器类（`AntiReverseController`）仅提供核心防逆流算法（`update()/reset()/isValidConfig()`），不处理通信与告警。若要求控制器内部实现这些保护，需扩展接口（增加通信状态、PCS 实际功率反馈、SOC 输入与告警/状态字输出）。

### 7. 测试验证方案

#### 7.1 仿真测试场景

表格

| 场景 | 初始条件 | 动作 | 预期结果 |
| --- | --- | --- | --- |
| 光伏突增 | 负载 100kW，光伏 50kW，P_grid=50kW | 光伏突然增至 200kW | 比例项当拍动作；1~2 拍内倒送停止，随后积分项消除残差并进入死区（稳态残差 ≤ deadband_enter，完整收敛约 50~70s，见第4章说明） |
| 负载突降 | 负载 200kW，光伏 250kW，P_grid=0（充电50kW） | 负载突然降至 100kW | 储能充电立即增加（比例项当拍动作），P_grid 无持续倒送，进入死区后 P_grid→0 附近、充电→150kW |
| 光伏波动 | 光伏在 50~150kW 间连续波动 | 连续波动 | 并网点功率总体 ≥0（斜坡率 50kW/s、Ts=0.1s 下峰值瞬时偏差 ≤100ms），储能充电功率跟随波动（前馈作用） |
| 无逆流时 | P_grid > 0 | 无逆流 | 控制器输出为 0，不干扰正常调度 |
| 光伏骤降（回归） | 充电稳定后（积分已积累） | 光伏骤降 | 充电快速退出至 0（陈旧积分清零保护），P_grid 回到购电侧，无长时间过充/振荡 |

#### 7.2 硬件在环测试

连接真实 PCS 模拟器和关口电表模拟器，验证通信延迟、控制周期和指令执行是否满足要求。