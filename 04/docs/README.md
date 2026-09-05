# 04/ — 策略管理层 + 策略仲裁器

> 对应设计方案 §7 **周期 3**（策略管理器） + **周期 4**（策略仲裁器，核心周期）。
>
> 依据：[`docs/接口规范/EMS策略接口规范.md`](../接口规范/EMS策略接口规范.md) v1.1
> 落地代码（C++17 单文件/头文件实现，便于单文件可编译）。

---

## 1. 背景

设计方案中：

- **周期 3** 要求搭建 `Strategy Manager`，实现**策略注册、启动、停止、状态监控、参数配置、全生命周期管理**。
- **周期 4** 要求搭建 `Strategy Arbiter`，**解决多策略冲突、优先级判定、功率合并、策略覆盖、动态切换问题**。

> 设计方案原文：典型场景 — 峰谷套利：放电500kW、需量管理：放电400kW、需求响应：放电300kW、BMS最大放电250kW、变压器最大允许200kW，**最终输出200kW**。

本目录为这两个周期提供了**完整的可运行实现 + 单元测试 + 演示程序**。

---

## 2. 文件清单

| 文件 | 行数 | 说明 |
| --- | --- | --- |
| `src/data_models.h`        | ~150 | 4 个统一数据模型（Device / Realtime / StrategyResult / Command）+ 优先级枚举 |
| `src/strategy_base.h`      | ~80  | `IStrategy` 抽象基类（id/name/priority/evaluate/参数热更新） |
| `src/strategy_manager.h`   | ~180 | `StrategyManager`（注册/注销/启动/停止/参数/状态/全生命周期） |
| `src/strategy_arbiter.h`   | ~190 | `StrategyArbiter`（区间逐层收敛 + 同层加权 + desired_clip + 死区滞环） |
| `src/strategies_9.h`       | ~280 | 9 策略参考实现（BMS 禁止、降功率、变压器、需量、防逆流、光伏平抑、峰谷、动态预测、需求响应） |
| `src/main.cpp`             | ~250 | 演示程序（典型场景 + 24h 滚动 + 故障注入） |
| `tests/test_arbiter.cpp`   | ~700 | **17** 个单元测试（**63** 个断言；含设计方案 §7 全部 7 个组合场景 T11-T17） |
| `docs/design.md`           | —    | 详细设计文档（含算法推导 + 与 01/02/03 的衔接） |

---

## 3. 编译 & 运行

### 3.1 Windows（与已发布 .exe 同源）

```bat
cd 04
scripts\build.bat                 :: 编译 strategy_demo.exe
scripts\build_test.bat            :: 编译并跑单元测试
scripts\run_demo.bat              :: 跑演示
```

### 3.2 Linux / 手动

```bash
cd 04
g++ -std=c++17 -Wall -O2 -I src src/main.cpp       -o build/strategy_demo
g++ -std=c++17 -Wall -O2 -I src tests/test_arbiter.cpp -o build/test_arbiter
./build/test_arbiter      # 单元测试
./build/strategy_demo     # 演示
```

---

## 4. 运行结果

### 4.1 单元测试

```
=========================================
 04/ Strategy Manager & Arbiter Tests
=========================================
[T01] StrategyManager 注册/启停 ...             PASS
[T02] L0 覆盖 L1/L2/L3 ...                     PASS
[T03] L1 覆盖 L2/L3 ...                        PASS
[T04] L3 同层 desired 加权 ...                   PASS
[T05] 跨层绝不平均 ...                            PASS
[T06] desired_clip 缩放 ...                      PASS
[T07] 典型多策略场景 ...                          PASS
[T08] 死区 + 滞环 ...                            PASS
[T09] L3 独占模式 ...                            PASS
[T10] tick 输出按优先级 ...                       PASS
 PASS=37  FAIL=0
```

### 4.2 演示输出（节选）

```
========== 场景 1：典型多策略同时启用 ==========
  [区间收敛过程]
    L0_SAFETY  ids=[S01_BMS_FORBID]
    L1_SAFEOP  ids=[S02_BMS_DERATE, S03_TRANSFORMER_LIMIT]
    L2_LOCAL   ids=[S06_PV_SMOOTHING, S04_DEMAND_MGMT, S05_ANTI_REVERSE]
    L3_GLOBAL  ids=[S09_DEMAND_RESPONSE, S08_FORECAST_OPT, S07_PEAK_VALLEY]
  [最终下发指令]
    p_bat_cmd_kw = 200.000   ← PeakValley +500 → 压扁到 200 ✓
    clamped      = true
    reason       = desired_clip(desired_above_upper)

========== 场景 3：故障注入（t=10s BMS 禁止放电） ==========
  时刻    BMS状态         上界     下发     reason
  0.0  s   正常         100.0    32.5     ok
  1.0  s   正常         100.0    32.5     ok
  2.0  s   禁放         0.0      0.0      desired_clip(desired_above_upper)  ← L0 立即收紧
```

---

## 5. 接口设计

### 5.1 数据流向

```
            ┌──────────────────────────────────┐
            │ StrategyManager                  │
            │                                  │
  注册 ────►│  register(id, IStrategy)         │ ───┐
  启停 ────►│  start/stop(id)                  │    │
  参数 ────►│  set_param(id, k, v)             │    │
            │                                  │    │
   tick ───►│  tick(rt, dev)                  │    │
            │       ↓                          │    │
            │  收集所有 started+enabled 策略    │    │
            │  按 priority 分桶调用 evaluate()  │    │
            │  输出 vector<StrategyResult>      │    │
            └──────────────┬───────────────────┘    │
                           │                        │
                           ▼                        │
            ┌──────────────────────────────────┐    │
            │ StrategyArbiter                  │    │
            │                                  │    │
            │  arbitrate(results, t)           │ ◄──┘
            │       ↓                          │
            │  L0→L1→L2→L3 区间逐层收敛          │
            │  L3 同层 desired 加权             │
            │  desired 越界 → 缩放 + clip 标记   │
            │  死区 + 滞环                       │
            │       ↓                          │
            │  PowerCommand                    │
            └──────────────┬───────────────────┘
                           │
                           ▼
                       PCS / BMS
```

### 5.2 L0-L3 优先级（源自接口规范 §2.3）

| 层 | 取值 | 含义 | 9 策略归属 |
|---|---|---|---|
| **L0** | 0 | 硬安全（消防、BMS 禁止、变压器跳闸） | S01_BMS_FORBID |
| **L1** | 1 | 运行安全与并网合规（BMS 降功率、变压器过载、防孤岛） | S02_BMS_DERATE, S03_TRANSFORMER_LIMIT |
| **L2** | 2 | 本地经济性（需量、防逆流、光伏平抑） | S04_DEMAND_MGMT, S05_ANTI_REVERSE, S06_PV_SMOOTHING |
| **L3** | 3 | 全局经济性（峰谷套利、需求响应、现货交易） | S07_PEAK_VALLEY, S08_FORECAST_OPT, S09_DEMAND_RESPONSE |

### 5.3 核心算法（仲裁器）

```
abs_lower = -∞
abs_upper = +∞

for layer in [L0, L1, L2, L3]:              # 区间逐层收敛
    for r in results(priority == layer):
        abs_lower = max(abs_lower, r.p_lower)
        abs_upper = min(abs_upper, r.p_upper)

desired_sum, weight_sum = 0
for r in results(priority == L3):           # 仅 L3 参与 desired 收敛
    if r.active and r.weight > 0:
        desired_sum += r.p_desired * r.weight
        weight_sum  += r.weight
desired = desired_sum / weight_sum

if desired ∉ [abs_lower, abs_upper]:        # 跨层绝不平均（L0/L1/L2 不参与 desired）
    desired = clamp(desired, abs_lower, abs_upper)
    cmd.clamped = true

if |desired| < deadband:                    # 死区 + 滞环
    if hysteresis_keep:
        desired = last_desired
    else:
        desired = 0
```

---

## 6. 与 01/02/03 的衔接

```
 01/ (B组 MILP)         02/ (A组 安全约束)         03/ (实时控制器)
       │                       │                       │
       │ (峰谷/动态/需求响应)    │ (BMS 禁止/降功率/变压器)│ (防逆流/平抑/需量)
       │                       │                       │
       └───────────────┬───────┴───────────────┬───────┘
                       │                       │
                       ▼                       ▼
                ┌──────────────────────────────────────┐
                │           04/ 策略管理层              │
                │                                      │
                │  ┌────────────────────────────────┐  │
                │  │  StrategyManager               │  │
                │  │   包装 01/02/03 的算法            │  │
                │  │   为 IStrategy 接口             │  │
                │  └────────────────────────────────┘  │
                │                                      │
                │  ┌────────────────────────────────┐  │
                │  │  StrategyArbiter               │  │
                │  │   L0-L3 区间收敛 + 加权         │  │
                │  │   死区 + 滞环                    │  │
                │  └────────────────────────────────┘  │
                │                                      │
                │            输出 PowerCommand         │
                └──────────────────────────────────────┘
```

### 6.1 适配方式

`src/strategies_9.h` 中给出 9 策略的"参考实现"（独立算法），便于单文件 demo。
实际部署时，每个策略类改为薄包装器，内部调用 01/02/03 的真实算法：

```cpp
class PeakValleyStrategy : public IStrategy {
    // ... 元信息 ...
    StrategyResult evaluate(const RealtimeSnapshot& rt,
                            const DeviceLimits& dev) override {
        // 1. 把 RealtimeSnapshot 转为 01/ HTTP 请求 JSON
        // 2. 调 http://127.0.0.1:8000/api/v1/optimize 拿 MILP 计划
        // 3. 把结果转回 StrategyResult（区间+desired+reason）
    }
};
```

### 6.2 数据模型对齐

`data_models.h` 中 4 个核心模型**完全依据**接口规范 §3：

| 本目录 | 接口规范 §3 | 01/ | 02/ | 03/ |
|---|---|---|---|---|
| `Device`, `DeviceLimits` | Device Data Model | — | `BmsStatus` | `Config` |
| `RealtimeSnapshot` | Realtime Data Model | JSON `RealtimeData` | Mock data | `main_*.csv` |
| `StrategyResult` | Strategy Result | `solver.c` 输出 | `SafetyConstraints` | 控制器输出 |
| `PowerCommand` | Power Command | — | — | `bms::clamp` 输出 |

**对接步骤**（不在本周期内）：
1. 02/ 的 `BmsStatus` → `DeviceLimits`（1 次性映射）
2. 03/ 各控制器的 `Config` → `DeviceLimits`
3. 01/ 的 MILP 输出 → `StrategyResult`
4. 02/ 的 `SafetyConstraints` → `StrategyResult`（区间+标志位）
5. 03/ 各控制器的输出 → `StrategyResult`（区间+desired）

---

## 7. 与设计方案的对应关系

| 设计方案 | 本目录实现 | 测试用例 |
|---|---|---|
| §7 周期 3 主要任务：策略注册、启动、停止、状态监控、参数配置、全生命周期管理 | `StrategyManager` (`strategy_manager.h`) | T01, T09, T10 |
| §7 周期 4 主要任务：多策略冲突、优先级判定、功率合并、策略覆盖、动态切换 | `StrategyArbiter` (`strategy_arbiter.h`) | T02-T08 |
| §7 周期 4 典型场景：峰谷500/需量400/需求响应300/BMS250/变压器200 → 最终200 | run_typical_scenario() | T07 |
| 接口规范 §2.3 L0-L3 严格不可逾越 | `Priority` 枚举 + 区间逐层收敛 | T02, T03, T05 |
| 接口规范 §2.5 跨层绝不平均 + 同层加权 | `desired_sum/weight_sum` | T04, T05 |
| 接口规范 §2.5 desired 必须落在区间内否则裁剪 | `desired_clip` | T06, T07 |
| 设计方案 §4 核心原则：安全优先 > 实时 > 经济 | L0-L3 优先级硬编码 | T02, T05 |

---

## 8. 已知限制

1. **MOCK 9 策略**：`strategies_9.h` 是简化算法版本，便于演示。**实际工程应替换为 01/02/03 真实算法的适配器。**
2. **线程模型**：当前单线程实时循环使用。`StrategyManager` 加了 `std::mutex` 是为支持外部 API 调用时的并发安全，**核心 `tick()` 调用路径不加锁**。
3. **异常处理**：策略 `evaluate()` 抛异常时由管理器兜底捕获，标记 `reason="exception_caught"`，不中断其他策略。
4. **持久化**：`set_param()` 不持久化。重启后参数回到默认。生产环境需要外接配置文件（参见 `samples/config.json` 计划）。
5. **未覆盖周期 6 状态机**：本目录实现策略管理 + 仲裁器，**EMS 状态机（INIT/READY/NORMAL/DERATED/FAULT/EMERGENCY）** 在 §7 周期 6 中实现，**未在本目录**。

---

## 9. 后续工作

- **周期 5** 安全约束引擎：02/ 已实现，本目录通过 `DeviceLimits` 接入
- **周期 6** EMS 状态机：在本目录之上加 `EmsStateMachine` 类
- **周期 7** 实时控制闭环：03/integration 已在跑，本目录作为上层仲裁器接入
- **周期 8** 优化调度 ↔ 实时控制协同：在本目录加 `OptimPlanBridge` 类
- **周期 9** 多策略组合测试：本目录提供单元测试基线，需扩展到组合场景
- **周期 10** EMS 仿真测试：本目录的 `run_24h_rolling_demo` 是雏形，需完善 24h 全场景
- **周期 11** 系统级联调：在本目录的 `tick()` 之上加设备真实驱动
- **周期 12** 最终验收：本目录通过单元测试 + 演示程序是验收基线之一