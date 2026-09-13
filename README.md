# 工商业储能 EMS —— 项目总览（BMS）

> 工商业储能 EMS（Energy Management System）的多模块协作参考实现。
> **A 组负责安全约束，B 组负责优化调度，三个实时控制器负责就地闭环，三者协同构成完整闭环。**

| | |
| --- | --- |
| 上层设计文档 | [`docs/architecture.md`](./docs/architecture.md) |
| 重构计划 | [`CHANGES.md`](./CHANGES.md)（已完成） |
| 协作模型 | A 组（安全保护）+ B 组（策略优化），上层调度对接 |
| 控制器时钟 | B 组优化：15 min 滚动；三个控制器：100 ms 实时 |
| 编程语言 | C / C++17 / Python 3（仅做图表） |
| 平台 | Windows（MinGW-w64 gcc/g++）+ Linux（求解器库 + 控制器可移植，仅 Windows HTTP 服务） |

---

## 1. 目录导览（新结构）

```
BMS/
├── README.md                                  ← 本文件
├── CHANGES.md                                 ← 重构变更记录
├── docs/
│   ├── architecture.md                        ← 顶层设计书（必读）
│   └── 产品化/
│       └── P0-架构分层.md                      ← 产品化 P0：算法 ↔ 设备解耦（IDeviceIO）
│
├── scripts/
│   └── build_all.bat                          ← 一键编译所有 C / C++ 组件
│
├── 01/                                       ← B 组 策略优化（纯 C · 自研 MILP · HTTP 服务）
│   ├── src/                                  ← .c / .h 源文件
│   ├── tests/                                ← 单元测试（test_solver.c）+ Python 校验
│   ├── samples/                              ← 示例请求 JSON
│   ├── bench/                                ← 96 时段性能基准脚本
│   ├── docs/                                 ← README + API 文档
│   ├── scripts/                              ← build.bat / run.bat
│   ├── requirements.txt                      ← Python 工具链依赖
│   └── build/                                ← 编译产物（git ignore）
│
├── 02/                                       ← A 组 安全约束（C++17 单文件骨架 + Mock + 单元测试）
│   ├── src/safety_constraint_manager.cpp     ← 主程序（演示 3 策略）
│   ├── tests/safety_test.cpp                 ← 11 个单元测试用例
│   ├── docs/README.md                        ← A 组模块说明
│   ├── scripts/                              ← build.bat / build_test.bat / run_demo.bat
│   └── build/                                ← 编译产物
│
├── 03/                                       ← 实时控制器（C++ · 100ms 闭环）+ 共享 + 集成
│   ├── shared/                               ← 控制器共享代码
│   │   ├── clamper.h                         ← 通用 bms::clamp（删除了三处副本）
│   │   └── controller_types.h                ← 类型别名
│   ├── anti_reverse_controller/              ← AntiReverseController + 5 个仿真场景
│   ├── pv_smoothing_controller/              ← SmoothingController + 6 个场景
│   ├── demand_management_controller/         ← DemandController + 4 个场景
│   └── integration/                          ← 三控制器集成（仅保留 IntegrationMain.cpp）
│
├── 04/                                       ← 策略管理层 + 仲裁器（C++17，对应周期 3 + 周期 4 + 周期 9）
│   ├── src/                                  ← data_models / device_io / strategy_base / manager / arbiter / strategies_9 + main.cpp
│   ├── tests/                                ← test_arbiter.cpp（17 用例 / 65 断言，全过；含设计方案 §7 全部 7 组合场景·单拍仲裁级）
│   ├── data/                                 ← demo 用的实时快照/电网参数 JSON
│   ├── samples/                              ← 受命令字序列样本（按 S01-S09 排序）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / run_demo.bat
│   └── build/                                ← strategy_demo.exe + test_arbiter.exe
│
├── 05/                                       ← 周期 5：统一安全约束引擎（C++17 头文件库）
│   ├── src/safety_engine.h                   ← 9 类约束 → 统一 (p_lower, p_upper) + 逐条 trace
│   ├── src/main.cpp                          ← 场景 A：19 个用例逐条触发对照表
│   ├── tests/test_safety_engine.cpp          ← T01~T06（68 断言）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / run_demo.bat
│   └── build/                                ← safety_demo.exe + test_safety_engine.exe
│
├── 06/                                       ← 周期 6：EMS 状态机（C++17 头文件库）
│   ├── src/state_machine.h                   ← 7 状态 / 8 类故障源 / 输出门控 / SOE / 急停锁存
│   ├── src/main.cpp                          ← 场景 B：全状态流转 + 门控真值表（纯状态机口径）
│   ├── tests/test_state_machine.cpp          ← T07~T10（34 断言）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / run_demo.bat
│   └── build/                                ← fsm_demo.exe + test_state_machine.exe
│
├── 07/                                       ← 周期 7：实时控制闭环 + 产品化 P0（C++17 头文件库）
│   ├── src/plant_model.h                     ← 被控对象：PCS 死区 + 惯性 + 变化率 + 效率 + 温升
│   ├── src/sim_device_io.h                   ← P0 仿真适配器：把 PlantModel 接到 IDeviceIO
│   ├── src/memory_device_io.h                ← P0.5 进程内点表适配器（RT_DB 的进程内等价物）
│   ├── src/realtime_loop.h                   ← EmsRuntime 11 步闭环 + OutputShaper + LoopMetrics
│   ├── src/main.cpp                          ← 场景 C：阶跃跟随 + 抖动治理三档对照 + 变化率对照
│   ├── tests/test_realtime_loop.cpp          ← T11~T16（6050 断言）
│   ├── tests/test_device_io.cpp              ← T21~T24（79 断言，P0/P0.5 适配器可换性）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / build_test_device_io.bat / run_demo.bat
│   └── build/                                ← loop_demo.exe + test_realtime_loop.exe + test_device_io.exe
│
├── 08/                                       ← 周期 8：优化调度与实时控制协同（C++17 头文件库）
│   ├── src/plan_loader.h                     ← 01/ MILP 计划 JSON 解析 + 贪心兜底规划器
│   ├── src/dispatch_coordinator.h            ← 滚动重优化 + 3 项实时纠偏 + PlanTrackingStrategy
│   ├── src/main.cpp                          ← 场景 D：24h 分层协同
│   ├── tests/test_dispatch_coordinator.cpp   ← T17~T20（444 断言）
│   ├── data/day_plan_sample.json             ← 01/ MILP 计划样例（96 点 × 15 min）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / run_demo.bat / gen_day_plan_sample.py
│   └── build/                                ← coord_demo.exe + test_dispatch_coordinator.exe
│
├── 09/                                       ← 周期 9：多策略组合测试（闭环时序级，C++17 头文件库）
│   ├── src/scenario_runner.h                 ← 7 场景定义 + 闭环运行器 + 稳态判定窗口
│   ├── tests/test_multi_strategy.cpp         ← T91~T97（77 断言，12000 拍/场景）
│   ├── docs/README.md                        ← 场景表 / 不变量口径 / 4 个真实缺陷复盘
│   ├── scripts/build_test.bat                ← 编译 + 运行
│   └── build/                                ← test_multi_strategy.exe
│
└── 10/                                       ← 周期 10：EMS 24h 离线仿真测试（C++17 头文件库）
    ├── src/day_curves.h                      ← 日曲线导入（CSV / 内置典型日）+ 曲线统计
    ├── src/econ_metrics.h                    ← 两部制经济性核算（需量窗口平均 + 衰减摊销）
    ├── src/sim_24h.h                         ← 24h 场景装配 + 故障时间窗注入 + 不变量校验
    ├── src/sim_report.h                      ← 产物导出：timeseries.csv / alarms.csv / summary.json / report.html
    ├── src/main.cpp                          ← 演示程序（典型日 / 故障注入日）
    ├── tests/test_sim_24h.cpp                ← T101~T110（133 断言）
    ├── data/typical_day_96.csv               ← 典型日曲线（96 点 × 15 min）
    ├── scripts/gen_curves.py                 ← 曲线生成器
    ├── docs/README.md                        ← 经济性口径 / 缺陷复盘 / 故障注入语义
    └── build/                                ← sim_demo.exe + test_sim_24h.exe
```

> **为什么周期 5~8 拆成 4 个模块**：`05/06/07/08` 与设计方案 §7 的四个周期**一一对应**，
> 每个模块都有自己的 `src/ tests/ data/ samples/ docs/ scripts/ build/`，可独立编译、独立测试、
> 独立跑演示。拆分的接口边界见 §3「模块依赖方向」。

> **关于 `code/` 目录**：那是用户并行进行的另一份 Python 实现（`ems/` 工程）的只读汇总视图，**不属于本仓库**，不要在 C/C++ 重构时碰它。

---

## 2. 一分钟快速开始

### 2.1 一键编译全部

```bat
scripts\build_all.bat
```

成功输出后各模块 `build\` 目录得到：

```
01\build\battery_server.exe                B 组 HTTP 服务（默认端口 :8000）
02\build\safety_constraint_manager.exe     A 组仿真演示
02\build\safety_test.exe                    A 组 11 个单元测试
03\anti_reverse_controller\build\ar_sim.exe            防逆流仿真
03\pv_smoothing_controller\build\smoothing_sim.exe  光伏平抑仿真（6 场景）
03\demand_management_controller\build\demand_sim.exe      需量仿真
03\integration\build\integration_sim.exe   三控制器集成仿真
04\build\strategy_demo.exe                 9 策略 + 仲裁器综合仿真（3 场景 + 故障注入）
04\build\test_arbiter.exe                   04/ 单元测试（17 用例 / 65 断言；含 §7 全部 7 组合场景·单拍仲裁级）
05\build\safety_demo.exe                    周期 5 场景 A：9 类约束逐条触发对照表
05\build\test_safety_engine.exe             05/ 单元测试（T01~T06 / 68 断言）
06\build\fsm_demo.exe                       周期 6 场景 B：全状态流转 + 门控真值表
06\build\test_state_machine.exe             06/ 单元测试（T07~T10 / 34 断言）
07\build\loop_demo.exe                      周期 7 场景 C：阶跃跟随 + 抖动治理 + 变化率对照
07\build\test_realtime_loop.exe             07/ 单元测试（T11~T16 / 6050 断言）
07\build\test_device_io.exe                 产品化 P0/P0.5 适配器可换性（T21~T24 / 79 断言）
08\build\coord_demo.exe                     周期 8 场景 D：24h 分层协同
08\build\test_dispatch_coordinator.exe      08/ 单元测试（T17~T20 / 444 断言）
09\build\test_multi_strategy.exe            09/ 单元测试（T91~T97 / 77 断言；7 场景 × 12000 拍闭环）
10\build\sim_demo.exe                        周期 10 场景 E：EMS 24h 离线仿真（产物 report.html 等 4 个文件）
10\build\test_sim_24h.exe                    10/ 单元测试（T101~T111 / 144 断言）
```

**全量回归：6961 断言全绿**（04=65 / 05=68 / 06=34 / 07=6050 / 08=444 / P0+P0.5=79 / 09=77 / 10=144），
`[BUILD ALL OK] All 20 components built.`

可选参数：`scripts\build_all.bat --no-test` 跳过 02/ + 04/ + 05~08/ 的单元测试（共 20 步 → 只跑 11 步）。

### 2.2 跑 B 组 C 策略服务

```bat
cd 01
scripts\build.bat
scripts\run.bat                       :: 启动 HTTP 服务，监听 :8000
:: 另开终端：
python tests\test_client.py samples\sample_request.json          :: arbitrage
python tests\test_client.py samples\sample_forecast.json         :: forecast
python tests\test_client.py samples\sample_demand_response.json  :: demand_response
python tests\verify_plan.py   samples\sample_request.json       :: 可行性校验
python tests\verify_optimum.py                                     :: 与 scipy 暴力枚举的最优性对照
python bench\bench_96.py                                           :: 96 时段性能基准
```

### 2.3 跑 A 组安全演示

```bat
cd 02
scripts\build.bat                     :: 编译演示主程序
scripts\build_test.bat                :: 编译并跑 11 个单元测试（应输出 ALL TESTS PASSED）
scripts\run_demo.bat                  :: 跑 9s 时序仿真，日志 → log\out.txt（默认 UTF-16 LE）
```

### 2.4 跑实时控制器

```bat
:: 单个控制器
cd 03\anti_reverse_controller        && scripts\run.bat
cd 03\pv_smoothing_controller        && scripts\run.bat
cd 03\demand_management_controller   && scripts\run.bat

:: 三控制器集成联调
cd 03\integration && scripts\run.bat
```

### 2.5 跑策略管理层 + 仲裁器（周期 3 / 周期 4 / 周期 9 产出）

```bat
cd 04
scripts\build.bat                     :: 编 strategy_demo.exe
scripts\build_test.bat                :: 编 + 跑 17 个仲裁器单元测试（应输出 PASS=63 FAIL=0）
scripts\run_demo.bat                  :: 跑 3 个综合场景 + 96 时段 24h 滚动 + 故障注入
```

`04/tests/test_arbiter.cpp` 覆盖范围：

- **基础层（10 用例，T01-T10）**：Manager 生命周期、L0/L1 覆盖、L3 同层加权、跨层不平均、desired_clip、死区/滞环、RunMode 独占、tick 顺序
- **§7 组合场景（7 用例，T11-T17）**：峰谷+BMS、峰谷+变压器、需量+防逆流、光伏平抑+防逆流、动态优化+需量、DR+峰谷+BMS、9 策略全量启用

> 04/ 是上层调度与就地控制之间的"策略管理层"——把 9 套不同优先级的策略统一起来，按 L0→L3 区间收敛 + 同层加权的方式产生单一下发指令，从而解决 §2.5 接口规范里的"多策略同时发声"问题。详细设计见 [`04/docs/design.md`](./04/docs/design.md)。

### 2.6 跑统一安全约束引擎（周期 5 产出）

```bat
cd 05
scripts\build.bat                     :: 编 safety_demo.exe + test_safety_engine.exe
scripts\build_test.bat                :: 编 + 跑 T01~T06（应输出 PASS=66 FAIL=0）
scripts\run_demo.bat                  :: 跑场景 A，输出 → build\demo_output.txt
```

场景 A 用 19 个用例逐条验证 9 类约束的区间语义与收敛结果：

```
统计：19 个用例 → L0 禁闭 9 个、L1 降额 6 个、紧急停机 3 个、区间矛盾 0 个
```

`05/tests/test_safety_engine.cpp` 覆盖范围（T01~T06）：

- 9 类约束逐条区间语义、区间求交 + binding trace、区间矛盾 → `[0,0]`+DERATED、变化率 slew limiter、并网区间推导、并网边界量测滤波

> 05/ 是**系统级安全边界统一器**：把 BMS 禁充放/降功率、SOC 上下限、PCS 限功率、变压器容量、
> 温度、变化率、并网（倒送与需量）全部折叠成**唯一** `(p_lower, p_upper)`，再作为 L0/L1 的
> `StrategyResult` 喂给 04/ 仲裁器。详细设计见 [`05/docs/design.md`](./05/docs/design.md)。

### 2.7 跑 EMS 状态机（周期 6 产出）

```bat
cd 06
scripts\build.bat                     :: 编 fsm_demo.exe + test_state_machine.exe
scripts\build_test.bat                :: 编 + 跑 T07~T10（应输出 PASS=34 FAIL=0）
scripts\run_demo.bat                  :: 跑场景 B，输出 → build\demo_output.txt
```

场景 B 覆盖 9 个阶段，并**实测**输出门控真值表（对每种状态实际构造一台状态机再读门控位）：

```
  状态      output_enabled strategies_enabled fail_safe
  INIT        禁止         禁止             是
  SELF_CHECK  禁止         禁止             是
  READY       禁止         允许             否
  NORMAL      允许         允许             否
  DERATED     允许         允许             否
  FAULT       禁止         禁止             是
  EMERGENCY   禁止         禁止             是

  合计迁移 13 次；状态构造不变量破坏 = 0
```

> 06/ 只做**状态判定 + 输出门控**，不参与功率计算。演示**纯状态机口径**（不引入 07/ 的
> `EmsRuntime`），保证周期 6 的交付物可独立验证。详细设计见 [`06/docs/design.md`](./06/docs/design.md)。

### 2.8 跑实时控制闭环（周期 7 产出）

```bat
cd 07
scripts\build.bat                     :: 编 loop_demo.exe + test_realtime_loop.exe
scripts\build_test.bat                :: 编 + 跑 T11~T16（应输出 PASS=6050 FAIL=0）
scripts\run_demo.bat                  :: 跑场景 C，输出 → build\demo_output.txt
```

场景 C 三个试验：

| 试验 | 验收要点（实测） |
| --- | --- |
| 1 阶跃跟随 | `无延迟=成立 无振荡=成立`；稳态段关口功率稳定在 0 附近 |
| 2 边界抖动治理 | 总行程 **4548 → 1326 kW（−70%）**、方向反转 **809 → 183（−77%）**，`无振荡` 由不成立转为**成立** |
| 3 变化率对照 | 50 vs 2000 kW/s：峰值倒送均为 148.0 kW，时长 2.8 vs 2.9 s（工程折中） |

> 抖动治理的关键结论：**根因在量测侧**（并网安全边界的基准量 `base = P_load − P_pv` 带噪声），
> 量测低通滤波是主药，输出死区+滞环是辅药。详细设计见 [`07/docs/design.md`](./07/docs/design.md)。

### 2.9 跑优化调度与实时控制协同（周期 8 产出）

```bat
cd 08
scripts\build.bat                     :: 编 coord_demo.exe + test_dispatch_coordinator.exe
scripts\build_test.bat                :: 编 + 跑 T17~T20（应输出 PASS=444 FAIL=0）
scripts\run_demo.bat                  :: 跑场景 D（24h），输出 → build\demo_output.txt
python scripts\gen_day_plan_sample.py :: 重新生成 01/ 计划样例（可选，已随仓库提供）
```

场景 D 24h 分层管控实测：

| 层 | 实测 |
| --- | --- |
| 优化层 | 初始计划取自 `01_plan_json`（`data/day_plan_sample.json`）；滚动重优化 **96 次**（每 15 min） |
| 实时层 | 平均绝对纠偏 **22.10 kW**（SOC 反馈 + 负荷前馈 + 窗口电量预算），上限 ±100 kW |
| 安全层 | 安全裁剪 **0 次**、状态门控 **0 次**、状态迁移 2 次 |
| KPI | 关口峰值 **350.0 kW**（契约 350 kW，未突破）；倒送 **0.0 kW / 0.0 s**；SOC ∈ [0.120, 0.844] |
| 闭环 | `mean_err=0.016 kW` `rmse=0.897 kW` `over_delivery=0.023%` `reversals=17` `travel=1326.686 kW` |

`08/tests/test_dispatch_coordinator.cpp` 覆盖范围（T17~T20）：计划 JSON 解析 + 采样、
贪心兜底优化器（含光伏余电裕度预留）、协调器纠偏有界 + 滚动重优化、24h 分层协同端到端。

> 08/ 建立「**优化层规划、实时层纠偏、安全层兜底**」的分层体系：优化层与实时层都**无法**
> 突破 05/ 的安全边界。详细设计见 [`08/docs/design.md`](./08/docs/design.md)。

### 2.10 产品化 P0：验证设备 I/O 抽象（新增）

```bat
cd 07
scripts\build_test_device_io.bat       :: 编 + 跑 T21~T24（应输出 PASS=79 FAIL=0）
```

| 测试 | 验证内容 |
| --- | --- |
| **T21** 适配器等价性 | 同一套算法，SimDeviceIO（物理模型）与 MemoryDeviceIO（纯点表）的指令/实际/SOC/关口序列**逐位一致**（maxΔ=0） |
| **T22** 点名 ↔ 结构体映射 | 写指令入点表、从点表装配快照、限制点读取、执行动力学、故障 fail-safe |
| **T23** `attach_device()` 生效 | 换适配器后限值随之改变（PCS 200kW → 50kW，指令被卡在 50kW） |
| **T24** 点表自检 | 30 个点齐全、闭环 200 拍无缺失点读取 |

> P0 是"能交付"与"只是实验室 demo"的分界线：从这一步起，算法不再直接持有 `PlantModel`，
> 而是通过 `IDeviceIO*` 读写设备。现场部署时只需 `attach_device()` 注入真实适配器（Modbus/RT_DB），
> **算法源码零改动**。详细设计见 [`docs/产品化/P0-架构分层.md`](./docs/产品化/P0-架构分层.md)。

### 2.11 跑多策略组合测试（周期 9 产出 · 闭环时序级）

```bat
cd 09
scripts\build_test.bat                 :: 编 + 跑 T91~T97（应输出 PASS=77 FAIL=0）
```

7 个场景各跑 **12000 拍（1200 s @ 10 Hz）**，真实策略 + 真实安全引擎 + 真实状态机 + 真实被控对象：

| ID | 场景 | 重点 |
| --- | --- | --- |
| T91 / S1 | 峰谷套利 + BMS 降功率 | BMS 降功率时峰谷指令不得越限（互相覆盖） |
| T92 / S2 | 峰谷套利 + 变压器过载 | `\|P_grid\| + 0.1·P_load` 不越变压器阈值 |
| T93 / S3 | 需量管理 + 防逆流 | 需量不突破 **且** 不倒送（两个 L2 从严合并） |
| T94 / S4 | 光伏平抑 + 防逆流 | 光伏剧烈波动下平抑与不倒送并存 |
| T95 / S5 | 动态优化 + 需量管理 | 优化层计划被需量约束正确压缩，计划跟踪不失效 |
| T96 / S6 | 需求响应 + 峰谷套利 + BMS 限制 | BMS 限值压过 DR/峰谷（跨层优先级） |
| T97 / S7 | 9 策略全量同时启用 | 全部策略 + 全部安全约束同时生效（压力测试） |

断言维度：指令逃逸 / 功率超限 / 门控不变量（**逐拍**硬判定）、双向充放电 / 频繁切换（时序）、
关口越界 / 变压器越限 / SOC 越界（**稳态**判定，排除门控期与 PCS 启动过渡）。

> **与 04/ T11~T17 不是重复**：那是单拍仲裁级（手工构造 `StrategyResult`，调一次 `arbitrate()`）；
> 本模块是闭环时序级 —— **单拍正确 ≠ 时序正确**。本周期实测暴露并修复了 4 个架构级缺陷
> （变化率区间从 `as_results()` 逃逸造成假区间矛盾、区间同步后为空、门控时权限区间未收成 `[0,0]`、
> 变压器约束的方向性错误 + 缺前馈）。详见 [`09/docs/README.md`](./09/docs/README.md)。

### 2.12 跑 EMS 24h 离线仿真（周期 10 产出）

```bat
cd 10
scriptsuild_test.bat                 :: 编 + 跑 T101~T110（应输出 PASS=133 FAIL=0）
scriptsun_demo.bat                   :: 跑典型日 + 故障注入日，产物落 build\
```

24 h = **86400 拍（dt = 1 s）**，装配 04~09 全栈，导入负荷 / 光伏 / 电价曲线与设备参数，
输出 4 个可交付产物：

| 产物 | 内容 |
| --- | --- |
| `timeseries.csv` | 逐拍时序 20 列（负荷 / 光伏 / 关口 / 指令 / 实际 / SOC / 温度 / 权限区间 / 原因） |
| `alarms.csv` | 告警日志（FSM SOE + 关口 + 变压器 + SOC + 安全限幅 + 温度 + 故障位） |
| `summary.json` | 机器可读汇总（曲线 / 经济 / 状态 / 不变量 / 告警 / 故障 / 配置） |
| **`report.html`** | **单文件可视化报告**：内联 SVG 折线、无 JS 依赖、离线可看可打印 |

**经济性（典型日，1000 kWh / 250 kW）**：日总电费 3622 元（无储能 4356 元），
节省 **734 元/日**，扣电池衰减 162 元/日，**净收益 572 元/日**，节省 16.9%，静态回收期 5.75 年。

演示程序参数：

```bat
build\sim_demo.exe                          :: 内置典型日 → build\
build\sim_demo.exe --csv data\typical_day_96.csv
build\sim_demo.exe --fault --out build\fault :: 故障注入日（PCS 故障 / BMS 通信中断 / 电表中断）
build\sim_demo.exe --log-every 1             :: 单拍粒度（排查瞬态必备）
```

> **不变量分两类**：**硬不变量**（指令逃逸 / 功率超限 / 门控）是 EMS 对设备端的契约，
> 任何场景（含故障日）都必须逐拍成立，违反即架构级问题；**安全不变量**（关口 / 变压器 / SOC）
> 是物理量，受**能量预算**约束 —— SOC 用尽即无削峰能力，越限是有效场景结论而非缺陷。
> 报告用 `grid_breach_soc_limited` 把可归因部分单独统计。
>
> **本周期修复了 2 个缺陷**：① 15 min 阶梯预报跳变导致关口瞬时倒送（单拍粒度下 1 拍 −7.9 kW，
> 10 拍粒度下被完全掩盖）—— 安全层并网上界引入"带可信度上限的下一拍前瞻"；
> ② 稳态判定窗口按日志行数比较，导致不变量结论依赖 `log_every` —— 改为按**时间**定义。
> 另得一个场景结论：故障日能量轨迹改变 → 储能提前触底 → 傍晚无容量削峰（220/220 拍可归因）。
> 详见 [`10/docs/README.md`](./10/docs/README.md)。

---

## 3. 架构层关系

```
上层调度 (15min)
    │
    ▼
02/A组  ──► SafetyConstraints (设备侧安全边界)
    │           │
01/B组  ──► OptimizedPlan ◄─┤
    │           │
    ▼           ▼
───── 04/ StrategyManager + StrategyArbiter (合并 9 策略 → 单下发指令) ─────
    │           │           │
    ▼ (100ms)
03/integration ◄── 03/anti_reverse_controller + 03/pv_smoothing_controller + 03/demand_management_controller
    │
    ▼
PCS / BMS
```

**05/06/07/08 补上"系统级"的那四层**（周期 5~8）：

```
   预测分析层 (15min)  96 点负荷/光伏/电价
        │
        ▼
   优化层   08/ 01-MILP 日间计划 ──► 每 15min 以实测 SOC 滚动重算（+ 贪心兜底规划器）
        │
        ▼
   实时层   08/ SOC 反馈 + 负荷前馈 + 窗口电量预算 → 100ms 有界纠偏（±100 kW）
        │
        ▼
   安全层   05/SafetyEngine：9 类约束 → 统一 (p_lower, p_upper)，逐条 trace 可定位
        │
        ▼
   状态层   06/EmsStateMachine：7 状态 / 8 类故障源 / 输出门控 / 急停锁存
        │
        ▼
   执行层   07/PlantModel：PCS 死区 + 惯性 + 变化率 + 效率，实际功率回灌下一拍
   设备 I/O  04/device_io.h：IDeviceIO 抽象接口（产品化 P0）
        │
        ▼
   ┌────┴────┬──────────────┬──────────────┐
   │ Sim     │ Memory       │ Modbus/RT_DB │  ← 适配器可换，算法零改动
   │ DeviceIO│ DeviceIO     │ / IEC104 ... │
   └─────────┴──────────────┴──────────────┘
```

> 一句话区分 02/ 与 05/：**02/ 是设备侧安全处理器，05/ 是系统级安全边界统一器** —— 前者回答"这台设备能不能动"，后者回答"整站这一拍最终允许多少功率"。

### 模块依赖方向（头文件层，无环）

```
  04/ ──► 05/ ──► 06/ ──┐
    │                   ├──► 07/ ──► 08/ ──► 09/ ──► 10/
    └───────────────────┴──────────────┘
        04/ 被所有模块复用（策略基类 / 数据模型 / 仲裁器 / **device_io**）
        09/ 是**验证层**（周期 9）：装配 04~08 全栈跑 12000 拍闭环时序
        10/ 是**装配层**（周期 10）：把全栈装进可配置的 24h 场景，产出交付物
        09/ 与 10/ 都不产出被其他模块依赖的头文件
```

| 模块 | 头文件搜索路径 | 说明 |
| --- | --- | --- |
| `05/` | `src` `../04/src` | 测试另需 `../06/src`（T03 交叉验证"矛盾→DERATED 仍允许输出"） |
| `06/` | `src` `../04/src` `../05/src` | 只用 `SafetyVerdict`，不依赖 07/ |
| `07/` | `src` `../04/src` `../05/src` `../06/src` `../08/src` | `EmsRuntime` 装配 08/ 的协同层；P0 新增 `device_io.h` |
| `08/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` | 演示与 T20 用 07/ 做端到端 |
| `09/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` | 只用 07/ 的 `EmsRuntime`，不反向被依赖 |
| `10/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` | 只用 07/ 的 `EmsRuntime`；曲线复用 08/ 的 `ForecastSeries` |

> `07/` 与 `08/` 互为**运行时调用关系**（闭环调用优化层 / 端到端用闭环），但**头文件层面无环**：
> `08/src/*.h` 不包含 `07/` 的任何头文件。这是 header-only 库的天然优势 —— 编译顺序无关，
> 只需把对应的 `-I` 路径都加上。

**关于 04/ 在闭环里的位置**：

- 9 个策略里，安全类（L0/L1）由 `02/` 提供基准约束；经济类（L3）里"基于优化的调度"由 `01/` 提供滚动计划（`ForecastOptStrategy` 接 `01/build` 出的 JSON 计划），其余"启发式经济策略"（`PeakValleyStrategy`、`DemandResponseStrategy`）就地实现。
- `StrategyManager::tick()` 按 `Priority` 桶里逐层调用 `evaluate()`；`StrategyArbiter::arbitrate()` 按 L0→L3 取 **max(p_lower)** / **min(p_upper)**、同层加权出 desired、然后 `desired_clip` —— 完全遵守 `docs/接口规范/EMS策略接口规范.md` §2.5。
- 最终唯一指令（`PowerCommand.p_bat_cmd_kw`）透出给 03/ 的就地控制器，由 100 ms 闭环继续做防逆流/平抑/需量二次修正。

---

## 4. 设计要点索引

- **总策略、安全边界、五大控制思想** → [`docs/architecture.md`](./docs/architecture.md)
- **A 组安全约束模块（BMS 禁止充放 / 降功率 / 变压器过载）** → [`02/docs/README.md`](./02/docs/README.md)
- **B 组策略服务（MILP 求解、JSON 协议）** → [`01/docs/README.md`](./01/docs/README.md)、[`01/docs/api.md`](./01/docs/api.md)
- **三控制器设计与集成** → [`03/anti_reverse_controller/docs/design.md`](./03/anti_reverse_controller/docs/design.md) 等各模块设计文档
- **策略管理层 + 仲裁器（9 策略 / L0-L3 / 周期 3+4）** → [`04/docs/README.md`](./04/docs/README.md)、[`04/docs/design.md`](./04/docs/design.md)
- **安全约束引擎（周期 5）** → [`05/docs/README.md`](./05/docs/README.md)、[`05/docs/design.md`](./05/docs/design.md)
- **EMS 状态机（周期 6）** → [`06/docs/README.md`](./06/docs/README.md)、[`06/docs/design.md`](./06/docs/design.md)
- **实时控制闭环（周期 7）** → [`07/docs/README.md`](./07/docs/README.md)、[`07/docs/design.md`](./07/docs/design.md)
- **优化调度与实时控制协同（周期 8）** → [`08/docs/README.md`](./08/docs/README.md)、[`08/docs/design.md`](./08/docs/design.md)
- **多策略组合测试（周期 9 · 闭环时序级）** → [`09/docs/README.md`](./09/docs/README.md)
- **EMS 24h 离线仿真测试（周期 10）** → [`10/docs/README.md`](./10/docs/README.md)
- **产品化 P0：算法 ↔ 设备解耦（IDeviceIO / SimDeviceIO / MemoryDeviceIO）** → [`docs/产品化/P0-架构分层.md`](./docs/产品化/P0-架构分层.md)
- **多策略协同的接口约定** → [`docs/接口规范/EMS策略接口规范.md`](./docs/接口规范/EMS策略接口规范.md)（§2.5 仲裁算法即 04/strategy_arbiter.h 的实现依据）

---

## 5. 代码组织原则

| 原则 | 说明 |
|---|---|
| **src/tests 分层** | 每个模块内部按 `src/` 源代码、`tests/` 测试、`samples/` 示例、`data/` 仿真 CSV、`figs/` 图、`docs/` 文档、`scripts/` 构建运行脚本、`build/` 产物分开。 |
| **共享代码唯一副本** | `03/shared/clamper.h` 是三个控制器共用 clamp，原本每处一份（共三份 + `ar_clamp` 模板），现在统一为 `bms::clamp`。 |
| **集成项目不复制源码** | `03/integration/src/IntegrationMain.cpp` 通过 `-I` 引用三个子项目的 header，源码不复制（原本有 6 份 `*.h/*.cpp` 重复，已删除）。 |
| **构建可重现** | 每个模块一个 `scripts/build.bat`；顶层 `scripts/build_all.bat` 串联全部。修改任何模块后只需重跑所在模块的 build.bat。 |
