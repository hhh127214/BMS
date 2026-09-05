# 周期 1–4 达成度审计报告

**审计对象**：12 周开发计划中 **周期 1 + 2 + 3 + 4** 的实际产出，对照 [`工商业储能EMS调控策略设计方案.md §7`](../工商业储能EMS调控策略设计方案.md) 各周期"主要任务 / 周期目标 / 输出"逐条核对。

**审计范围**：
- 周期 1 主要产出物（接口规范）：`docs/接口规范/EMS策略接口规范.md` v1.1（864 行）
- 周期 2–4 主要产出物（统一接口 + 管理器 + 仲裁器）：`04/src/` 7 文件（1445 行）+ `04/tests/test_arbiter.cpp`（17 用例 / 63 断言）+ `04/docs/`

**审计结论**：周期 1 / 3 / 4 **基本达成**，周期 2 **功能达成但字面对齐度不足**，详见下文。

---

## 一、周期 1：现有 9 个策略全面梳理

### 设计要求

> A：整理9个策略的输入、输出、参数、控制周期、启动条件、停止条件、优先级、异常条件。
> B：整理现有BMS、PCS、电表、光伏、变压器、EMS内部数据。
> 共同：建立《EMS 数据及策略接口规范 V1.0》
> 周期目标：完成 9 个策略标准化定义。
> 输出：9策略设计表、数据字典、策略接口规范。

### 实际情况

| 项 | 状态 | 证据 |
|---|---|---|
| 9 策略 I/O / 参数 / 周期 / 启停 / 优先级 / 异常 | ✅ 已涵盖 | 接口规范 §4（4.1–4.9 共 9 卡片，每片包含"基本属性 / 控制周期 / 启动 / 停止 / 异常"五段） |
| BMS / PCS / 电表 / 光伏 / 变压器 / EMS 数据 | ✅ 已涵盖 | 接口规范 §3.1 Device Data Model + §3.2 Realtime Data Model；02/ 提供 SOE/限功率事件录入链路 |
| 《EMS 数据及策略接口规范 V1.0》 | ✅ 完成 | 已升级至 **v1.1**，864 行（v1.1 回应 6 项评审：优先级 Q1、仲裁 Q2、降级 Q3、设备整合 Q4、时序 Q5、状态持久化 Q6） |
| 9 策略标准化定义 | ✅ 完成 | 接口规范 §4.10 《9 策略优先级总览》一表 |

### ⚠️ 偏差与遗留

1. **策略编号顺序不统一**
   - 设计方案 §3：S01=BMS禁止, S02=BMS降功率, S03=变压器, S04=需量, S05=防逆流, S06=光伏平抑, S07=峰谷, S08=动态预测, S09=DR
   - 接口规范 §4.10：S01=峰谷, S02=动态预测, S03=custom_script, S04=需量, S05=防逆流, S06=光伏平抑, S07=变压器, S08=SOC寿命, S09=BMS
   - 04/ `strategy_id` 命名：跟随**设计方案**（kBmsForbid…kDemandResponse），与接口规范 §4.10 不对齐
   - 影响：跨模块阅读时两个"第 1 号"会指向不同策略，对未来代码评审埋雷
   - **建议**：明确把"单一来源"（SSOT）写进 v1.2 修订记录，并在根 README 里说明 04/ 跟 design、其余模块跟 spec

2. **设计方案要求的 raw "数据字典"未单独成文**
   - 设计方案要求"输出：9 策略设计表、数据字典、策略接口规范"——三份独立文档
   - 当前只有"策略接口规范"一份，**数据字典已并入接口规范 §3**（字段级表）
   - 风险评估：低（信息完整），但与方案字面交付物不匹配

### 周期 1 达成度：**90%**（功能完整 / 字面交付物略不同）

---

## 二、周期 2：9 个策略统一接口

### 设计要求

> A：将现有 9 个策略封装成统一策略模块，统一输出：**strategy_id、strategy_name、state、direction、target_power、max_power、min_power、priority、timestamp、reason**。
> B：建立统一数据结构体：**RealtimeData、DeviceState、BatteryState、GridState、StrategyRequest、PowerCommand**。
> 周期目标：实现 9 策略 → 统一接口 → 统一数据模型。

### 实际情况

#### A 项：统一输出字段

| 设计要求字段 | 04/ 实际字段 | 语义对应 | 状态 |
|---|---|---|---|
| `strategy_id` | `strategy_id` (string) | 直接对应 | ✅ |
| `strategy_name` | `name()` (策略元方法，不在 result 中) | 类名携带，需补 | ⚠️ |
| `state` | `active` (bool) + `reason` | 语义等价（active=false ≈ idle/silent） | ⚠️ 等价 |
| `direction` | （由 `p_lower/p_upper` 符号推断） | 信息冗余可省 | ⚠️ 字段取消 |
| `target_power` | `p_desired` | 同义 | ⚠️ 改名 |
| `max_power` | `p_upper` | 同义 | ⚠️ 改名 |
| `min_power` | `p_lower` | 同义 | ⚠️ 改名 |
| `priority` | `priority` (Priority enum) | 直接对应 | ✅ |
| `timestamp` | （在 `RealtimeSnapshot::timestamp`，指令 `PowerCommand::timestamp` 携带） | 渠道不同 | ⚠️ 位置不同 |
| `reason` | `reason` (string) | 直接对应 | ✅ |

**结论**：5/10 字段字面一致，5/10 改名但语义等价。**功能完整，名称契约未严格对齐 design §7 周期 2 字面要求**。

#### B 项：统一数据结构体

| 设计要求结构 | 04/ 实际结构 | 状态 |
|---|---|---|
| RealtimeData | `RealtimeSnapshot` | ✅ 等价 |
| DeviceState | `Device` + `DeviceLimits`（拆为静态+运行时） | ⚠️ 拆 2 个 |
| BatteryState | （折合到 `DeviceLimits::pcs_*` + `RealtimeSnapshot::soc/soh/temperature_c`） | ⚠️ 未独立成体 |
| GridState | （折合到 `RealtimeSnapshot::p_grid_kw`） | ⚠️ 未独立成体 |
| StrategyRequest | （无；设计为策略入参，04/ 用 evaluate(rt, dev) 入参替代） | ⚠️ 取消 |
| PowerCommand | `PowerCommand` | ✅ |

**结论**：6 拆 4，独立结构体数 **减少 2 个**。功能上都有载体，但结构粒度变了。

### ⚠️ 偏差与遗留

1. **字段命名不严格匹配 design**——`target_power` vs `p_desired`、`max_power` vs `p_upper`。需要决定：是要把 design §7 周期 2 当作"必须遵从的字面契约"，还是当作"方向性指引"。
2. **BatteryState / GridState 未独立成体**——现折合到 DeviceLimits / RealtimeSnapshot 内部字段；功能完整但失去"设备类型维度"建模（如有并网逆变器/储能柜双向建模会更难扩展）。
3. **StrategyRequest 取消**——现实上"策略入参"被 evaluate 函数的形参列表取代，这是一个**主动的设计简化**，与方案有偏差但有理由。

### 周期 2 达成度：**85%**（接口/数据均落地，命名契约未字面对齐 design）

---

## 三、周期 3：策略管理器

### 设计要求

> 搭建 Strategy Manager，实现策略**注册、启动、停止、状态监控、参数配置、全生命周期管理**。
> 周期目标：实现所有策略可被 EMS 统一管控。

### 实际情况

| 设计要求功能 | 04/ 接口 | 状态 |
|---|---|---|
| 注册 | `register_strategy(unique_ptr/shared_ptr<IStrategy>)` + `unregister_strategy(id)` | ✅ |
| 启动 | `start_all()` + `start(id)`（单策略 + 批量） | ✅ |
| 停止 | `stop_all()` + `stop(id)` | ✅ |
| 状态监控 | `get_status(id)` + `get_all_status()` 返回 `StrategyStatus{priority, last_*, param_map}` | ✅ |
| 参数配置 | `set_param(id, key, value)` + `get_param(id, key)` 热更新 | ✅ |
| 全生命周期管理 | `on_register` / `on_enable` 钩子 + `enable/disable` + L3 独占 `set_run_mode` | ✅（额外加成） |

### 加分项

- **L3 RunMode 独占**：当上层模式切换为 MPC / DemandResponse 时，其它 L3 策略 `weight` 自动归零，避免多头套利
- **按优先级桶 tick 输出**：策略结果按 L0→L3 自然排序，便于仲裁器消费
- **线程安全**：用 `std::mutex` 守护状态（set_param / tick / arbitrate 链路一致）

### 周期 3 达成度：**100%**（全部达成且超出方案预期）

---

## 四、周期 4：策略仲裁器（核心周期）

### 设计要求

> 搭建 Strategy Arbiter，解决**多策略冲突、优先级判定、功率合并、策略覆盖、动态切换**问题。
> 典型场景：峰谷套利放电500 / 需量400 / DR300 / BMS_max 250 / 变压器_max 200 → 最终 200。
> 周期目标：完成多策略统一决策能力落地。

### 实际情况

| 设计要求功能 | 04/ 算法步骤 | 状态 |
|---|---|---|
| 多策略冲突 | `desired_clip(desired_above_upper / desired_below_lower)` + `interval_contradiction` fallback | ✅ |
| 优先级判定 | L0→L3 逐层 `abs_lower = max`/`abs_upper = min` 区间收敛 | ✅ |
| 功率合并 | 仅 L3 同层 `desired_sum += p_desired * weight` | ✅ |
| 策略覆盖 | 上层区间收紧下层区间（L0 收紧 L1/L2/L3 全局） | ✅ |
| 动态切换 | `set_deadband(kw)` + `set_switch_delay(n)` 双参数调 | ✅ |

测试矩阵（17 用例 / 63 断言，全过）：

| 用例 | 覆盖设计要点 | 断言数 | 结果 |
|---|---|---|---|
| T01 | 管理器生命周期（注册/启停） | 3 | ✅ |
| T02 / T03 | L0 / L1 覆盖（L0 区间、L1 区间收敛） | 6 | ✅ |
| T04 / T05 | L3 同层加权 / 跨层不平均 | 7 | ✅ |
| T06 | desired_clip（desired 超区间 → 裁剪） | 4 | ✅ |
| **T07** | **典型场景：峰谷500/需量/DR/BMS/变压器 → 200** | **4** | **✅** |
| T08 | 死区 + 滞环 | 3 | ✅ |
| T09 | L3 独占 RunMode | 4 | ✅ |
| T10 | tick 输出按优先级排序 | 3 | ✅ |
| T11–T17 | 多策略组合（周期 9 提前补） | 29 | ✅ |

### ⚠️ 偏差与遗留

1. **T07 典型场景未显式触发"变压器=200"约束**——当前 T07 用 `pcs_rated_dis_kw=200` 替代 `transformer_capacity_kw=200` 作为最终 200 上界生成者。功能上 200 输出正确，但**Transformer 策略在"设计原典典型场景"里未被实际钉死为限功率源**。
   - 建议：调整 T07 fixture，使用 `transformer_capacity_kw=200` + 让负载/光伏触发变压器过载逻辑，验证 Transformer 策略作为限功率源的链路

2. **"动态切换"指代模糊**
   - 04/ `deadband + hysteresis` 实现的是"符号方向切换不抖动"
   - 如果 design "动态切换"还包括"上层 RunMode 切换时的策略启停"，那**状态机（周期 6）才补全**——目前 04 仅做了 L3 RunMode 维度

3. **异常降级路径**——文档提及的"BMS 通信丢失按最保守 [0,0] 处理"已落到 §6，但 04/ 代码里依赖 `bms_*_forbidden` 显式入参，没有真实通信断线仿真。**这是周期 1 评审决议 Q3 + 周期 6 状态机应该处理的部分**。

### 周期 4 达成度：**95%**（核心算法全到位，T07 触发的限功率源应改用 Transformer）

---

## 五、综合打分

| 周期 | 字面达成 | 功能达成 | 关键障碍 | 综合分 |
|---|---|---|---|---|
| 1 | 90% | 100% | 数据字典未独立成文、策略编号 design/spec/04 三方不齐 | **90%** |
| 2 | 80% | 95% | 字段命名未严格遵从 design 字面；BatteryState/GridState 未独立 | **85%** |
| 3 | 100% | 100% | 无 | **100%** |
| 4 | 95% | 100% | T07 限功率源应改 Transformer 而非 PCS | **95%** |
| **综合** | — | — | — | **92%** |

---

## 六、建议的下一步治理动作（不动代码级别）

按风险 / ROI 排序：

1. **🔴 必做：统一策略编号 SSOT** —— 在 `docs/SSOT.md`（新增）写明"以设计 §3 为 SSOT，接口规范 §4.10 与 04/ 均引用该 SSOT 的别名"，避免后续 03/05/06 接口继续分叉
2. **🟡 建议：补对齐 battery_state.h / grid_state.h 两个 model header**，根据设计字面契约分拆，为周期 6 状态机做准备（电池 SOH、电网 alarm 维度不可再揉在 RealtimeSnapshot）
3. **🟡 建议：T07 fixture 改用 Transformer 触发**，让 §7 周期 4 典型场景字面严格执行一次
4. **🟢 可选：v1.2 接口规范升级**，把 `Direction` / `TargetPower` 别名也写入 StrategyResult，正式承认 04/ 的字段命名

---

## 七、附录：审计覆盖的代码与文档清单

```
docs/接口规范/EMS策略接口规范.md            864 行  周期 1 主产出
04/src/data_models.h                       220 行  周期 2 A/B 数据模型
04/src/strategy_base.h                      92 行  周期 2 A 策略基类
04/src/strategy_manager.h                  236 行  周期 3 全部功能
04/src/strategy_arbiter.h                  194 行  周期 4 全部功能
04/src/strategies_9.h                      438 行  9 策略参考实现（覆盖 §3.1–3.9）
04/tests/test_arbiter.cpp                  ~870 行 17 用例 / 63 断言
04/docs/README.md                                     模块手册
04/docs/design.md                                   详细设计文档
04/src/main.cpp                           265 行  3 场景 demo

总计：04/ 1 文件夹 + 1445 行 C++ 实现 + 864 行规范文档 + 17/63 测试用例
```
