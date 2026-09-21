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
│   ├── 产品化/
│   │   └── P0-架构分层.md                      ← 产品化 P0：算法 ↔ 设备解耦（IDeviceIO）
│   └── 规划/
│       └── 模拟器与设备接入梳理.md              ← 接口方向 / 两层点表 / 指令下行路径 / 缺口清单 §10
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
├── 07/                                       ← 周期 7：实时控制闭环 + 产品化 P0 / RT_DB 接入（C++17 头文件库）
│   ├── src/plant_model.h                     ← 被控对象：PCS 死区 + 惯性 + 变化率 + 效率 + 温升
│   ├── src/sim_device_io.h                   ← P0 仿真适配器：把 PlantModel 接到 IDeviceIO
│   ├── src/memory_device_io.h                ← P0.5 进程内点表适配器（RT_DB 的进程内等价物）
│   ├── src/rtdb/rtdb_device_io.h             ← RT_DB 接入：共享内存实时库适配器 + 设备侧写点器
│   ├── src/rtdb/ems_point_table.h|.c         ← 40 点 EMS 点表（32 设备侧 + 8 EXT；点名与 mem_point:: 一致）
│   ├── src/rtdb/ext_setpoints.h              ← ★ 读侧：EXT 外部设定的读取 + 失效判定（A3.1）
│   ├── src/rtdb/ems_rt_db_setup.h|.c         ← 点表初始化器（建段 + 注册 40 点）
│   ├── src/realtime_loop.h                   ← EmsRuntime 11 步闭环 + ⓪ 限值刷新 + OutputShaper + LoopMetrics
│   ├── src/main.cpp                          ← 场景 C：阶跃跟随 + 抖动治理三档对照 + 变化率对照
│   ├── vendor/rt_db/                         ← RT_DB 源码快照（api .c/.h + structs + private）
│   ├── tests/test_realtime_loop.cpp          ← T11~T16（6050 断言）
│   ├── tests/test_device_io.cpp              ← T21~T24（79 断言，P0/P0.5 适配器可换性）
│   ├── tests/test_rtdb_device_io.cpp         ← T25~T29（546 断言，RT_DB 接入：跨内存边界闭环等价）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test.bat / build_test_device_io.bat / build_test_rtdb.bat / run_demo.bat
│   └── build/                                ← loop_demo.exe + test_realtime_loop.exe + test_device_io.exe + test_rtdb_device_io.exe
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
├── 10/                                       ← 周期 10：EMS 24h 离线仿真测试（C++17 头文件库）
│   ├── src/day_curves.h                      ← 日曲线导入（CSV / 内置典型日）+ 曲线统计
│   ├── src/econ_metrics.h                    ← 两部制经济性核算（需量窗口平均 + 衰减摊销）
│   ├── src/sim_24h.h                         ← 24h 场景装配 + 故障时间窗注入 + 不变量校验
│   ├── src/sim_report.h                      ← 产物导出：timeseries.csv / alarms.csv / summary.json / report.html
│   ├── src/main.cpp                          ← 演示程序（典型日 / 故障注入日）
│   ├── tests/test_sim_24h.cpp                ← T101~T110（133 断言）
│   ├── data/typical_day_96.csv               ← 典型日曲线（96 点 × 15 min）
│   ├── scripts/gen_curves.py                 ← 曲线生成器
│   ├── docs/README.md                        ← 经济性口径 / 缺陷复盘 / 故障注入语义
│   └── build/                                ← sim_demo.exe + test_sim_24h.exe
│
├── 11/                                       ← 周期 11：系统级联调（**跨进程**，设备侧 / EMS 侧分离）
│   ├── src/integration_runner.h              ← Pacing 节拍器 + DeviceSideSim + EmsSideApp + IntegrationCheck
│   ├── src/main_initializer.cpp              ← 共享内存段初始化器（建段 + 注册 40 点 + 常驻保活）
│   ├── src/main_device.cpp                   ← 设备侧进程（BMS/PCS/电表/光伏/变压器/负荷六类数据源）
│   ├── src/main_ems.cpp                      ← EMS 侧进程（05 安全 + 06 状态机 + 07 闭环 + 08 协同 + P2 可观测）
│   ├── tests/test_system_integration.cpp     ← T41~T49（151 断言）
│   ├── scripts/run_integration.bat           ← 三进程联调一键跑
│   └── docs/README.md                        ← 联调报告：任务→检查项映射 / 故障源角色边界 / 4 个缺陷复盘
│
└── 12/                                       ← 周期 12：最终验收（交付门禁，对上游**纯消费者**）
    ├── src/acceptance_runner.h               ← 七维度验收运行器（A1~A7，55 检查项，纯头文件）
    ├── src/main_acceptance.cpp               ← CLI 入口（打印 + 落盘三件套 + 门禁退出码）
    ├── tests/test_acceptance.cpp             ← T51~T59（逐维度断言 + 报告自洽性 + JSON 独立反解）
    ├── scripts/run_acceptance.bat            ← 正式验收（含落盘与退出码）
    ├── docs/README.md                        ← 怎么跑 / 报告怎么看 / 三个验收结论 / 已知限制
    ├── docs/design.md                        ← 验收设计说明（为什么这样验）
    ├── docs/ACCEPTANCE-REPORT.*              ← 验收报告（运行后生成：md / json / html）
    └── build/                                ← acceptance.exe + test_acceptance.exe

vendor/                                       ← 第三方库（**只增不改**，不参与重构）
    └── lib60870/                             ← IEC 60870-5-104 协议栈（v2.4.1，C99，GPL-3.0）
        ├── src/ config/ examples/            ← 上游源码快照，一行未改
        ├── build.bat                         ← 编静态库 lib60870.a（gcc -std=gnu99）
        ├── README.md                         ← 来源 / commit / 编译参数 / 两个坑 / GPL 说明
        └── README-upstream.md                ← 上游原 README（改名保留）

P3/                                           ← 产品化 P3：IEC104 从站网关（对上接调度 / 虚拟电厂）
    ├── src/iec104_point_map.h                ← ★ 唯一 IOA 真相源（24 上送 + 6 下行 + self_check + CSV 覆盖）
    ├── src/iec104_server.h                   ← 从站封装：GI 状态机 / ASDU 分包 / 品质三态 / SBO / 统计
    ├── src/iec104_time.h                     ← 把库的 CP56Time2a 从 UTC 口径扳回北京时间
    ├── src/rtdb_quality.h                    ← RT_DB 品质真值引用 + 编译期对账 + C11 原子宏擦除
    ├── src/rtdb_source.h                     ← RtDbReadOnlySource（**只有 read()，没有 write()**）
    ├── src/ext_setpoint_sink.h               ← ★ 窄接口：六个具名 setter + IOA 分发（写不出 CMD.*）
    ├── src/rtdb_ext_sink.h                   ← ★ RtDbExtWriter：唯一写 EXT 区的适配器 + 发布协议
    ├── src/main_gateway.cpp                  ← 网关进程（三档受理：默认拒 / DRY-RUN / 落 EXT 区）
    ├── src/main_master.cpp                   ← 主站模拟器（对端工具，需库内部头）
    ├── tests/test_iec104.cpp                 ← T61~T72 + T77（协议 + EXT 判定 + 转发表 CSV，不需要 RT_DB）
    ├── tests/test_ext_rtdb.cpp               ← T73~T76（**跨共享内存边界**的落点测试）
    ├── data/point_map_default.csv            ← 转发表默认表的 CSV 形态（--point-map 用；现场改它不重编）
    ├── scripts/                              ← build.bat / build_test.bat / run_gateway.bat
    └── docs/                                 ← README.md（实测基线 / 现场部署 / 10 个缺陷复盘）+ design.md

P1/                                           ← 产品化 P1：配置化（现场部署不改源码，只改配置）
    ├── src/json_lite.h                       ← 最小 JSON 解析/生成（零依赖；支持注释 / 尾逗号 / 裸键）
    ├── src/ems_config.h                      ← 配置模型 + 字段绑定表（Binder）
    ├── src/config_loader.h                   ← load / save / validate / apply / capture（装配顺序唯一入口）
    ├── src/config_doc.h                      ← 配置模板 / Markdown 文档 / Schema 生成
    ├── src/main.cpp                          ← 演示程序 ems_config.exe
    ├── tests/test_config.cpp                 ← T201~T218（171 断言）
    ├── data/ems_config.sample.json           ← 现场配置样例（带注释）
    ├── docs/README.md                        ← 装配顺序陷阱 / 校验规则 / 测试清单
    └── build/                                ← ems_config.exe + test_config.exe

P2/                                           ← 产品化 P2：可观测性（现场出问题能看见）
    ├── src/soe.h                             ← 统一事件记录 SOE + 时间窗抑制（838 拍风暴 → 2 条）
    ├── src/metrics.h                         ← 增量指标注册表（counter / gauge / histogram，O(1)）
    ├── src/trace.h                           ← 跟踪等级 + 采样间隔（按子系统独立，热更新）
    ├── src/observe.h                         ← RuntimeObserver：接入 EmsRuntime 的唯一入口
    ├── src/main.cpp                          ← 演示程序 ems_observe.exe
    ├── tests/test_observe.cpp                ← T301~T315（434 断言）
    ├── docs/README.md                        ← 为什么需要 P2 / 两个核心机制 / 事件分级 / 现场用法
    └── build/                                ← ems_observe.exe + test_observe.exe

13/                                           ← Modbus TCP 设备接入（**真的过网线**：EMS 作主站）
    ├── src/modbus_tcp_client.h               ← 协议层：MBAP/PDU 编解码 / 事务 / 超时分类 / 串包自愈
    ├── src/modbus_point_map.h                ← ★ 映射唯一真相源（32 点 / 4 条编译期守卫 / 分块表）
    ├── src/modbus_device_io.h                ← IDeviceIO 第 4 个实现（现场那个）
    ├── src/fake_modbus_slave.h               ← 进程内测试从站（**可注入故障**：半包/串包/异常/静默）
    ├── src/main_probe.cpp                    ← 现场点表核对工具 modbus_probe.exe
    ├── tests/test_modbus_tcp.cpp             ← T01~T15 协议层（223 断言，不需要 Python）
    ├── tests/test_modbus_device_io.cpp       ← T21~T30 适配器契约（133 断言，不需要 Python）
    ├── tests/test_modbus_bridge.cpp          ← T40~T47 跨语言联调（83 断言，**需 pymodbus**）
    ├── sim/modbus_slave.py                   ← Python 从站（pymodbus）+ 设备模型 + 32 点 TSV 导出
    ├── scripts/                              ← build.bat / build_test.bat / run_sim.bat
    └── docs/                                 ← README.md（怎么跑 / 三层测试 / 6 个缺陷复盘）+ design.md
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
07\build\test_rtdb_device_io.exe            RT_DB 接入（T25~T29 / 546 断言，共享内存实时库）
08\build\coord_demo.exe                     周期 8 场景 D：24h 分层协同
08\build\test_dispatch_coordinator.exe      08/ 单元测试（T17~T20 / 444 断言）
09\build\test_multi_strategy.exe            09/ 单元测试（T91~T97 / 77 断言；7 场景 × 12000 拍闭环）
10\build\sim_demo.exe                        周期 10 场景 E：EMS 24h 离线仿真（产物 report.html 等 4 个文件）
10\build\test_sim_24h.exe                    10/ 单元测试（T101~T111 / 144 断言）
11\build\rtdb_initializer.exe                 周期 11 共享内存段初始化器（段 + 全点表注册，常驻保活）
11\build\device_side.exe                      周期 11 设备侧进程（六类数据源 + 故障剧本）
11\build\ems_side.exe                         周期 11 EMS 侧进程（全栈闭环，消费共享内存）
11\build\test_system_integration.exe         11/ 单元测试（T41~T49 / 151 断言）
12\build\acceptance.exe                       周期 12 最终验收器（七维度 A1~A7 / 55 检查项 / 门禁退出码）
12\build\test_acceptance.exe                  12/ 模块自测（T51~T59 / 逐维度 + 报告自洽性）
P1\build\ems_config.exe                      产品化 P1 配置化：校验 / 装配 / 往返 / 模板 / 文档 / 导出
P1\build\test_config.exe                     P1/ 单元测试（T201~T218 / 171 断言）
P2\build\ems_observe.exe                      产品化 P2 可观测性：汇总 / 解耦验证 / 故障 SOE / 导出
P2\build\test_observe.exe                     P2/ 单元测试（T301~T315 / 434 断言）
P3\build\iec104_gateway.exe                   产品化 P3 IEC104 从站网关（对上接调度 / 虚拟电厂）
P3\build\iec104_master.exe                    P3 主站模拟器（对端工具：对钟 / 总召唤 / 遥调 / 遥控）
P3\build\test_iec104.exe                      P3/ 单元测试（T61~T72 + T77 / 408 断言；**不需要** RT_DB）
P3\build\test_ext_rtdb.exe                    P3/ EXT 落点测试（T73~T76 / 121 断言；**会 reset 整个段**）
13\build\modbus_probe.exe                     13/ 现场点表核对工具（读 32 点 / 读限值 / 下发指令）
13\build\test_modbus_tcp.exe                  13/ 协议层 T01~T15（223 断言；不需要 Python）
13\build\test_modbus_device_io.exe            13/ 适配器契约 T21~T30（133 断言；不需要 Python）
13\build\test_modbus_bridge.exe               13/ 跨语言联调 T40~T47（83 断言；**需 Python + pymodbus**）
vendor\lib60870\build\lib60870.a              IEC 60870-5-104 协议栈静态库（gcc -std=gnu99）
```

**全量回归：9304 断言全绿（脚本默认口径，`exit=0`）**；把 `pymodbus` 环境建起来后
是 **9382**（`02/` 只印 `ALL TESTS PASSED` 不印计数，故不入明细 /
04=65 / **05=94** / 06=34 / 07=6050 / P0+P0.5=81 / **RT_DB=554** / 08=444 / 09=77 / 10=144 /
P1=171 / P2=434 / **11=152** / 12=114 / **P3=529**（协议 408 + EXT 落点 121）/
**13=439**（默认环境 361）），
`[BUILD ALL OK] All 31 components built.`

> **★ 13/ 让基线有两个合法值**（环境依赖，必须写清用哪个口径）：
>
> | 条件 | `13/` 贡献 | 全量基线 | `13/` 收尾行 |
> | --- | --- | --- | --- |
> | 没有装 `pymodbus` 的 Python（**开箱默认**） | 361 | **9304** | `[SKIP] … 跨语言层未运行` |
> | 装了 `pymodbus`（`13\.venv` 或 `EMS_PYTHON`） | 439 | **9382** | `[OK] … 三层测试全部通过` |
>
> **判据**：`grep "SKIPPED=0" build_all.log` 命中 → 9382 口径；命中 `SKIPPED=7` → 9304 口径。
> 桥接测试**总是**打印 `SKIPPED=n`（哪怕是 0）—— 只在 skip 时才打印的话，
> "第三层确实跑了"在日志里就**没有正面证据**，只能靠"没有那一行"去推断，
> 而"缺行"与"grep 写错 / 日志被截断"完全无法区分。
>
> 跨语言层需要**外部 Python + pymodbus**，环境缺失时以 0 退出（环境问题不是代码缺陷），
> 但 `build_test.bat` 的收尾行会**据实**改印 `[SKIP]`，不会仍然声称"三层全部通过"。
> 建环境：`python -m venv 13\.venv` + `pip install -r 13\sim\requirements.txt`，
> 然后设 `EMS_PYTHON`（`13/scripts/build_test.bat` 也会自动捡起 `13\.venv`）。

> **断基数口径**：15 项明细加总 = 9382。三种**打印格式不同**，扫描时要认全：
>
> | 格式 | 谁 |
> | --- | --- |
> | 不印计数（只印 `ALL TESTS PASSED`） | `01/ 03/` 各步、**`02/`**、`04/` 的 demo 步 |
> | 中文格式 `通过 N / 失败 M` | **`P1/`**（171）、**`P2/`**（434） |
> | `PASS=N FAIL=M` / `PASS=N` | 其余全部（注意**有的只印 PASS 不印 FAIL**） |
>
> 只 `grep PASS=` 会漏掉 P1/P2 的 **605** 条。`02/` 在明细里写成 `02 /`（不带 `=n`）。
> **按模块归集**可用 `python logs/_tally.py build_all.log`（临时脚本，各步一列）。
>
> **基线演进**：`12/` 验收加入后 8333 → P3 追加后 **8501** →
> A1 修复后 **8523**（`07/` RT_DB 533 +14 / `11/` 143 +8）→
> A2 修复后 **8547**（`07/` RT_DB 546 +13 / `07/` P0+P0.5 81 +2 / `11/` 151 +8 / `12/` 114 +1）→
> **2026-09-20 B1 新增 `13/` 模块后 8984**（新模块 437，其余不动；
> **开箱默认环境跑出来是 8906** —— 跨语言层 7 条 SKIP）→
> **2026-09-20 A3.1 后 9245 / 9323**（`P3/` 168 → **497**（+329）/
> `07/` RT_DB 546 → **554**（+8，点表 32 → 40 点后 T25 逐点循环多跑 8 次）/
> `13/` 221 → **223**（+2，EXT 区边界断言）；**其余不动**）→
> **2026-09-20 A3.2 后 9276 / 9354**（`05/` 68 → **94**（+26，第 10 条约束 T07）/
> `P3/` 协议 376 → **380**（+4，T72 停机粘住）/ `11/` 151 → **152**（+1，设备侧跳过 EXT）；
> **其余不动**）→
> **2026-09-20 转发表外置 CSV 后 9304 / 9382**（`P3/` 协议 380 → **408**（+28，T77 转发表 CSV 加载）；
> **其余不动**）。

`12/` 的主产物是**验收报告**而非断言计数：七维度 **55 检查项**，实跑结论
`55 / 55 项通过`，退出码 0。

`P3/` 的验收不止断言：还有**端到端两阶段实测** —— 设备在线全 `GOOD`、
设备停止全 `NT`，且 `0x80` 坏点 **0 条**。

> **判据修正（2026-09-19，见 `CHANGES.md` §23）**：`11/` T47 原先断言
> `stale_reads() == 0`。全量构建（本机满载）下实测 `降级=22 / 1068810 次读`
> 而**同一二进制单跑恒为 0** —— 这条判据测的是"机器闲不闲"，不是"代码对不对"。
> 现改为四层判据：①`碰撞 > 0`（反向守卫）②`降级 < 碰撞`（碰撞必须被重试吸收）
> ③`降级*1000 < 读次数`（量级守卫）④`半写状态 0`（硬判据）。
> `stale` 的语义是**设计内降级**（保留上次有效值），不是故障，本来就不该恒零断言。
> `11/` 因此由 **133 → 135 断言**；由 `device_pump` 同线程顺序驱动的那些
> `stale == 0`（`07/` T2x、`11/` T41~T45、`12/` A5-04）**保留不动** —— 那里它是确定性的。
>
> **安全输入通路修复（2026-09-19，见 `CHANGES.md` §25）**：BMS 禁充放位原先在
> `RtDbDeviceIO::read_limits()` 里**硬写 `false`**，等于"BMS 永远允许充放"——
> L0 最硬的安全边界在实时库形态下**永不触发**。修复时连带发现**更严重的一条**：
> `EmsRuntime::step()` 从不调用 `refresh_device_limits()`，**运行期限值冻结**，
> 于是 BMS 动态降功率 / PCS 额定 / 变压器容量 / 契约需量全部退化成装配期常量。
> 两条都补了三层断言（`07/T25`、`11/T48`、`12/A5-07`）。

> **安全量取错源修复（2026-09-19，见 `CHANGES.md` §26）**：同一个 `p_grid_kw`，
> 算法路径用 `p_load - p_pv - p_bat` **推算**、记录路径读**电表点** `EMS_P_GRID`。
> 真实系统里电表是**唯一权威计量点**，三路相减是把误差**叠加**；两个消费者
> （S05 防逆流、变压器过载）都是安全约束。已收敛成 `PlantModel::meter_p_grid()`
> **一处定义**，三个适配器统一读电表点，并修掉"电表读数比其它量测慢一拍"。
> ★ **差点修了个寂寞**：设备侧发布的 `MEAS.P_GRID` 恰好等于那个相减式，
> 改完在既有夹具下**逐位恒等** —— 所以先加了**默认 0、可注入**的电表系统偏差
> 把两条路径拉开，契约才有区分度。回归：`07/T29`、`11/T49`、`12/A1-09`
> （退回推算式 → 立刻 6 条红）。

可选参数：`scripts\build_all.bat --no-test` 跳过 02/ + 04/ + 05~08/ + 07(RT_DB)/ + 09/ + 10/ + 11/ + 12/ + P1/ + P2/ + P3/ + **13/** 的单元测试。

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
| **T24** 点表自检 | 32 个点齐全、闭环 200 拍无缺失点读取 |

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
scripts\build_test.bat                 :: 编 + 跑 T101~T110（应输出 PASS=133 FAIL=0）
scripts\run_demo.bat                   :: 跑典型日 + 故障注入日，产物落 build\
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

### 2.13 配置化：现场部署不改源码（产品化 P1 产出）

```bat
cd P1
scripts\build_test.bat                    :: 编 + 跑 T201~T218（应输出 通过 171 / 失败 0）
scripts\run_demo.bat                      :: 6 步完整演示，产物落 build\
```

**目标**：现场部署只改配置文件，不动算法源码。

```bat
build\ems_config.exe --demo                      :: A/B 对照：只改配置，行为随之改变
build\ems_config.exe --check  data\ems_config.sample.json
build\ems_config.exe --apply  data\ems_config.sample.json
build\ems_config.exe --roundtrip data\ems_config.sample.json
build\ems_config.exe --dump-template > site.json :: 带注释的模板，改完即可用
build\ems_config.exe --dump-doc      > CONFIG.md
build\ems_config.exe --dump-schema   > schema.json
build\ems_config.exe --capture build\captured.json :: 导出运行中实际生效的参数
```

配置覆盖 **89 个字段**（6 个 section：`loop` / `plant` / `limits` / `safety` / `coordinator` /
`state_machine`）+ 策略条目（`id → enabled / note / params`）。缺省字段沿用结构体默认值，
一份现场配置只描述"这个站和默认有什么不同"。

**为什么需要 P1**：装配过程有 **3 处隐式顺序依赖**，都不会在编译期报错 ——

| 陷阱 | 机制 | 不知道会怎样 |
| --- | --- | --- |
| ① `configure_plant()` 重置 `dev_` | 内部 `read_limits()` 首行 `out = DeviceLimits{}` | 配置里的 `transformer_capacity_kw` / `d_target_kw` 被**静默复位成默认值 250** |
| ② `apply_configs()` 依赖 `dev_` | 用 `dev_.pcs_rated_*` 算协同层设备参数 | 优化层按**旧设备**排 96 点计划 |
| ③ `OutputShaper` 缓存副本 | `init()` 存的是**值**不是引用 | 改了死区参数，**行为不变** —— "配置生效了但没生效" |

P1 把这份知识收进 **`apply_config()` 一个函数**。T211/T212/T213 分别**同时验证反例与正例**
（例如 T211 先演示"错误顺序 → limits 被冲掉"，再验证 `apply_config()` 顺序正确）。

**A/B 对照实测**（24 h / 86400 拍，只改 `transformer_capacity_kw` `d_target_kw` `grid_p_max_kw`：630 → 200）：

| 指标 | 基线(630) | 收紧(200) | 变化 |
| --- | --- | --- | --- |
| 关口峰值 | 434.0 kW | 292.1 kW | **−141.9 kW** |
| 储能充电量 | 538.6 kWh | 3.2 kWh | −535.4 kWh |
| 储能放电量 | 365.8 kWh | 192.9 kWh | −172.9 kWh |
| 指令逃逸 | 0 拍 | 0 拍 | 硬不变量保持 |

> **两个设计要点**：① **字段绑定表**（`Binder`）同时驱动 load / save / 模板 / 文档 / Schema，
> 避免"手写两份映射必然漂移"；T208 断言字段数 == 89，漏绑即失败。
> ② **策略 id 打错必须报错** —— 静默忽略会让人以为"配了但没生效"。本模块自带的样例配置
> 第一次就写错了 id（应为 `S04_DEMAND_MGMT` 而非 `demand_mgmt`），被 `--check` 当场拦下。
> 详见 [`P1/docs/README.md`](./P1/docs/README.md)。

### 2.14 可观测性：现场出问题能看见（产品化 P2 产出）

```bat
cd P2
scripts\build_test.bat                   :: 编 + 跑 T301~T315（应输出 通过 434 / 失败 0）
scripts\run_demo.bat                     :: 4 步完整演示，产物落 build\ 与 out\
```

**目标**：现场出问题时，能在 30 秒内回答"发生了什么、什么时候、多严重"，
而不是去翻 8 万行时序 CSV。

```bat
build\ems_observe.exe --demo 24            :: 24h 闭环 + 观察者汇总（SOE / 指标 / 分位）
build\ems_observe.exe --decouple           :: 指标与 log_every 无关（核心价值验证）
build\ems_observe.exe --fault 12           :: 故障场景 SOE（置位/清除事件对）
build\ems_observe.exe --export out 24      :: 导出 soe.csv / soe.json / metrics.prom / metrics.json
```

**为什么需要 P2**：P2 之前可观测性散在三个模块、三种表示，且**在生产环境不可用** ——

| 位置 | 表示 | 问题 |
| --- | --- | --- |
| `06/state_machine.h` | `StateEvent{ts, from, to, reason}` | 只有状态迁移，没有等级 / 事件码 |
| `10/src/sim_24h.h` | `AlarmEntry` + `collect_alarms()` | **告警组装寄生在仿真装配层**（入参是 `Sim24hConfig`）→ **现场部署后系统没有告警能力** |
| `07/realtime_loop.h` | `StepRecord` 向量 | 是时序不是事件；`LoopMetrics::compute(log_, …)` 是 **O(N) 全量重算**，24h 日志 8.6 MB **只增不减** |
| `07/realtime_loop.h` | `cycle_us_sum_ / max_` | 只有均值与最大值，**没有直方图 / 分位数** |

**两个核心机制**：

| 机制 | 说明 |
| --- | --- |
| **边沿检测 → SOE** | 事件是"状态的变化"，不是"每拍的值"。一次 838 拍的限幅 = **2 条**事件（Start/End），不是 838 条。替代了 `10/` 里手工维护的 5 个 `in_xxx` 标志。**例外**：状态迁移与硬不变量违例 `suppressible=false`，合并会丢掉迁移链（`A→B→C` 变成 `A` 重复 2 次） |
| **时间窗抑制** | 同 `(source, code)` 在 `suppress_window_s` 内合并为一条，带 `repeat_count` / `duration_s`。合并时保留**绝对值更大**的字段值，不丢峰值 |
| **逐拍累积 → 指标** | O(1) 更新，内存有界，**与 `log_every` 无关** |

**核心价值实测**（`--decouple`，3600 拍，负荷在 300/50 kW 之间每 5 拍切换）：

| log_every | 观察者行程 | 观察者换向 | 日志行数 | `07/` 日志行程 |
| --- | --- | --- | --- | --- |
| 1 | 88477.8 kW | 719 | 3600 | 88477.8 kW |
| 10 | **88477.8 kW** | **719** | 360 | **0.0 kW** |

`log_every=10` 时 `07/` 报的指令总行程是 **0**，真实值 **88477.8 kW** —— **完全失明**；
观察者不受影响。这正是周期 10 那个缺陷的根因：**为了省资源而调大 `log_every`，
等于同时关掉了故障可见性。**

**三个正交的旋钮**（替代单一 `log_every`）：

| 维度 | 载体 | 现场用法 |
| --- | --- | --- |
| ① **关键事件** | `SoeLog` | 永远记，不受任何降采样影响（838 拍限幅 → 2 条） |
| ② **跟踪等级** | `TraceControl::set_level()` | 按子系统决定细节量，运行期热更新。**故障级（`>= kError`）永不受等级限制** |
| ③ **采样间隔** | `TraceControl::set_sample_every()` | 高频跟踪按子系统采样，**不影响 SOE** |

**`--fault` 实测**（12 h，3 个故障时间窗）：43200 拍 → **18 条事件（2400:1 压缩）**，
每条故障成对（`PCS_FAULT_SET` / `PCS_FAULT_CLEAR`），每条迁移都带 `reason`
（如 `DERATED → FAULT (fault_in_derated:PCS_FAULT)`）。

**有界内存与自省**：`size()` / `dropped()` / `total()` / `suppressed()` 四个数一起看 ——
可观测性组件**必须能报告自己的数据丢失**，否则"什么都没输出"到底是"真的没事"
还是"缓冲区爆了"分不清。

> **两个设计要点**：① **状态迁移不自己推导，直接读 `rt.fsm().history()`** —— 迁移自带
> `reason`，且自己用 `prev_state_` 做边沿检测会**漏掉第一拍的 `INIT → SELF_CHECK`**。
> ② **不修改 `07/`** —— 观察者外挂，`obs.on_step(rt, rec)` 是唯一接入点。
> 详见 [`P2/docs/README.md`](./P2/docs/README.md)。

---

### 2.15 RT_DB 接入：换数据源不改算法（从进程内到跨进程）

```bat
cd 07
scripts\build_test_rtdb.bat    :: 期望 PASS=546 FAIL=0 / ALL TESTS PASSED
```

P0.5 用 `MemoryDeviceIO` 证明了「换数据源不改算法」在**进程内**成立；现场要换的是
**另一个进程**。本步把共享内存实时库 RT_DB 接进 `IDeviceIO`（`RtDbDeviceIO`），
方向边界是：**设备侧只写 `MEAS/STA/CFG`，EMS 侧只写 `CMD`**，算法层看不到任何点名。

**核心证据（T26）**：同一套 `EmsRuntime`，一路数据全经共享内存（`RtDbDeviceIO` +
设备侧泵），一路直连进程内点表（`MemoryDeviceIO`），装配序列/环境脚本完全相同 ——
**400 拍记录逐位一致**（含 `p_cmd` / `p_actual` / `p_grid` / `soc` / 区间 / 状态 / reason）。

| 用例 | 证明什么 |
| --- | --- |
| T25 | 共享内存点表 ↔ 编译期点表 ↔ 设备侧点表**三方一致**（40 点的点名/单位/索引 + 常量漂移守卫）；含 **BMS 禁充放位四方向**（置位 / 不串扰 / 可恢复 / 双置） |
| T26 | 跨内存边界闭环**逐位等价**（峰值指令 100 kW，SOC 0.5→0.4993） |
| T27 | 两个独立连接（两次 `rt_db_init`）看到**同一段内存**（写 A 读 B / 写 B 读 A） |
| T28 | 设备侧写 `STA.PCS_FAULT` → EMS 进 FAULT → 指令归零并撤销许可 → 恢复后停在 READY **不自动带载** |
| T29 | **关口功率单一数据源**：给电表注入 **+40 kW** 系统偏差，判定 `p_grid_kw` 取的是**电表**而非三路相减的平衡值，且 `read_actuals()` 同口径（缺口 A2） |

> **现场三个约束**（本次接入踩出来的）：① Windows 下段是页面文件映射对象，
> **初始化器不能是「跑完就退」的短命进程**（`ems_rt_db_setup` 会保留存活句柄）；
> ② 初始化器必须在所有连接之前调用（`reset` 会清空段级元数据）；
> ③ 一个进程一条连接（RT_DB 的 open/create 共用一个进程级全局句柄）。
> 详见 [`07/docs/README.md`](./07/docs/README.md) §8。

---

### 2.16 系统级联调：三个进程 + 一个共享内存段（周期 11 产出）

```bat
cd 11
scripts\run_integration.bat    :: 三进程联调（初始化器 + 设备侧 + EMS 侧），期望 8/8 检查项通过
scripts\build_test.bat         :: 单进程测试 T41~T49，期望 PASS=151 FAIL=0
```

周期 1~10 的交付物都是**单进程**粒度的。本步是第一个**跨进程**模块：把同一个
`EmsRuntime` 拆成设备侧进程与 EMS 侧进程，中间只通过 RT_DB 共享内存段通信 ——
物理上不可能共享内存指针，因此"换数据源不改算法"这句承诺在这里经受最严苛的检验。

```
rtdb_initializer.exe ── 建段 + 注册 40 点 + 常驻保活（Windows 段存活依赖）
device_side.exe      ── 六类数据源（BMS / PCS / 电表 / 光伏 / 变压器 / 负荷）+ 故障剧本
ems_side.exe         ── 05 安全 + 06 状态机 + 07 闭环 + 08 协同 + P2 可观测
```

| 用例 | 证明什么 |
| --- | --- |
| T41 | 权限区间 `(p_lower, p_upper)` 经共享内存往返**不变号**（负值也不丢） |
| T42 | 六类数据源的量测链路**全部活着**（含温度随功率变化，不是死值） |
| T43 | 跨进程闭环**真闭环**（充放能量非零、状态机不抖动、指令均值误差可控） |
| T44 | 六类故障源**角色边界**正确（设备侧只对自己能判定的源做 fail-safe，EMS 侧做全局兜底）；故障清除后停在 `READY` **不自动带载** |
| T45 | 24 h 长稳（**86400 拍**跨共享内存闭环，三段故障窗）全程 `stale_reads() == 0` 且无"指令越界"告警 —— 这里由 `device_pump` **同线程顺序**驱动，`stale` 是确定性的 0，硬判据成立 |
| T46 | 日志记录全流程：SOE 成对、迁移链完整、观察者步数与 `log_every` **解耦** |
| T47 | 并发抗碰撞：**真读写并发**下 seqlock 碰撞必须被**重试吸收**（`降级 < 碰撞`），半写状态一次都不得漏出 —— 判据是统计性的，理由见下 |
| T48 | **BMS 禁充放位**经共享内存驱动安全区间：窗内 **0 拍放电**（7 层判据含差分守卫）、不串扰充电、禁放只封上界 |
| T49 | **关口功率单一数据源**：注入 **+40 kW** 电表系统偏差，200 拍判定 `p_grid_kw` 跟**电表**、两路径逐拍一致、gap 恒 = 40（缺口 A2） |

> **跨进程联调暴露并修掉了 07/ 的四个真实缺陷**：① `EmsRuntime::step()` 从不调用
> `write_command()` → 权限区间恒 `[0,0]` → 3000 拍里 2978 拍被判"指令越界"；
> ② `RtDbDeviceIO::refresh()` 把 seqlock 碰撞（语义是"请重试"）误计为采集失败 →
> 600 拍里 51 次假告警；③ `MemoryDeviceIO` 缺热模型 → `MEAS.T_C` 恒 25℃；
> ④ **设备限值在运行期是冻结的**（`step()` 从不刷 `read_limits()`），BMS 动态降功率 /
> PCS 额定 / 变压器容量 / 契约需量全退化成装配期常量。
> 四者都在单进程测试里**永远不会暴露**。另有一条由**代码复核**发现、在 11/ 补了
> 跨进程判据的缺陷（缺口 A2，关口功率两个口径，§26 / §2.15 的 T49）。
> 完整复盘见 [`11/docs/README.md`](./11/docs/README.md) §5。

> **一个刻意的"不修"**：`07/` 的 L2 需量纠偏存在 **1 拍极限环**（`p_pred_avg` 里含
> "假设维持当前关口功率"，而"当前关口"取决于本拍刚下发的指令；`merge_realtime_correction()`
> 无滞环）。修它会动到 `05/` `08/` `09/` 的 7500+ 条断言基线，代价与收益不匹配 ——
> 改为把联调基准调到可行工况（1000 kVA / 600 kW），并把缺陷与 4 组对照实验数据
> 记入 [`11/docs/README.md`](./11/docs/README.md) §7.1。

---

### 2.17 最终验收：七维度交付门禁（周期 12 产出）

```bat
cd 12
scripts\build.bat              :: 编译验收器
scripts\run_acceptance.bat     :: 正式验收（七维度 + 落盘三件套 + 门禁退出码）
scripts\run_acceptance.bat --no-write --quiet    :: 只打印、不落盘、只显示汇总与失败项
scripts\build_test.bat         :: 模块自测 T51~T59
```

`12/` 是**交付门禁，不是新功能**：它对上游是**纯消费者**（不新增算法、不修改任何模块、
不读私有成员），只把前面 11 个周期交付的能力按设计方案的七个维度**重新测量一遍**。

**为什么不能直接引用既有测试的结论**：开发期断言断言的是"我实现的东西符合我的实现"，
判据来自实现细节；验收断言的是"交付物符合设计方案的字面要求"，判据来自公开出口
（`StepRecord` / 点表 / 文件 / 墙钟）。两者口径不同 —— 所以 `12/` **不复用断言，只复用能力**。

| 维度 | 检查项 | 验收对象 |
| --- | --- | --- |
| **A1 功能** | 8 | 策略注册/启停、全链路闭环、状态机七态可达、设备 I/O 三适配器等价、点表自检、配置化往返、日计划装载 |
| **A2 策略** | 9 | 9 个策略逐个在"触发工况"下核对设计意图（§3.1~§3.9） |
| **A3 安全** | 11 | 9 条安全约束逐位注入、区间矛盾语义、急停锁存、全域硬不变量 |
| **A4 性能** | 5 | 单拍耗时（均值/峰值/占用率）、24h 离线仿真墙钟加速比、长跑资源无泄漏 |
| **A5 稳定性** | 7 | 24h 长稳（三段故障窗）、跨进程闭环长稳、stale 零、SOE 零丢弃、迁移与抖动有界、可观测性解耦、BMS 禁充放位通路 |
| **A6 多策略协同** | 8 | 周期 9 的 7 个组合场景（闭环时序级）全通过 + 汇总 |
| **A7 文档** | 6 | 根文档、模块 `docs/README.md`、模块构建脚本、统一构建入口步数自洽、根 README 索引、报告落盘 |

每条检查项都带三要素：**编号 / 判据（人话）/ 实测（数值）**，因此任何一项都能被单独
定位与复核。所有可标定阈值集中在 `AcceptanceRunner::Options` 一处 —— 现场标定只改这里。

**本机实测结论**（`--root .. --quiet`）：

```
结论: 通过  ——  55 / 55 项通过，0 个维度未全通过

单拍耗时均值/峰值/占用率 : 9.44 / 326.6 us / 0.00944 %
24h 墙钟 / 加速比        : 0.99 s / 87192×
24h 经济净收益           : 734.17 元（等效循环 0.7297 次）
跨进程 stale / 越区间    : 0 / 0
SOE 事件 / 丢弃          : 8 / 0
```

产物三件套写在 `12/docs/`：`.md`（人读）、`.json`（CI 抓 `summary.passed/total/result`）、
`.html`（汇报，双击即开）。JSON 是**真的 JSON** —— T58 用 `P1/src/json_lite.h`
这个**独立解析器**反解验证，不是自证。CI 直接：

```bash
build/acceptance.exe --quiet --root .. || exit 1     # 0 = 通过 / 1 = 不通过 / 2 = 参数错误
```

> **一个必须理解的验收结论**：满充（禁充，下界 = 0）叠加光伏大发（不许倒送，上界 < 0）时，
> 本拍无可行非零功率 —— 安全引擎把区间收成 `[0,0]` 并判 **`DERATED`（可自恢复）**，
> **绝不锁存 `EMERGENCY`**。因为 `EMERGENCY` 需要人工复位，一旦锁存储能会在整个光伏高发
> 时段失去调节能力。检查项 **A3-09** 就是这条决策的回归护栏。详见
> [`12/docs/README.md`](./12/docs/README.md) §5。

### 2.18 IEC104 从站网关：对上接调度（产品化 P3 产出）

```bat
cd P3
scripts\build_test.bat         :: 协议层自环测试 T61~T68（不需要 RT_DB，2 秒跑完）
scripts\build.bat              :: 编网关 + 主站模拟器
scripts\run_gateway.bat        :: 自检 + 监听 2404 + 回环演示（需要 RT_DB，先起初始化器）
build\iec104_gateway.exe --self-check-only                     :: 上电首查
build\iec104_gateway.exe --port 2404 --ca 1 --stale-ms 5000    :: 生产启动
```

**EMS 是 IEC 104 的从站（服务器），调度是主站（客户端）。** 前面所有模块都在解决
"站内怎么算"，`P3/` 解决"**外面怎么看见它**"：

```
   调度主站 ──TCP:2404──► P3 网关 ──只读──► RT_DB 共享内存段（11/ 立起来的）
   （客户端）              （从站）              ▲
                                        device_side / ems_side
```

三条设计纪律，每条都用**类型**而不是约定来表达：

| 纪律 | 怎么表达 |
| --- | --- |
| 网关**只读** RT_DB，不写任何点 | `RtDbReadOnlySource` **没有 `write()` 方法** |
| 调度的遥控不能绕过安全层 | 命令只走 `ICommandSink` → 装配层裁决，**默认一律否定确认** |
| 换算系数不许散在代码里 | 集中在 `iec104_point_map.h` 一张表的 `scale`/`offset` 列 |

**点表**：30 个内部点 → **24 上送 + 6 下行**。IOA 区间本项目自定
（`1001` 遥信 / `4001` 遥测 / `5001` 遥控 / `6001` 遥调 / `8001` 累计量），
**现场必须按调度下发的转发表改** —— 改的只有这一个文件里的一张表。

其中 **IOA 4009/4010「允许功率上界/下界」是本项目的独有价值**：调度不只看到
"现在发了多少功率"，还能看到"EMS 还允许调到哪个范围"，不用试探着下发才知道边界。

**品质三态（本轮的设计改动）**：`IDataSource::read()` 从 `bool* quality_ok` 改成
`Quality*`，因为 IEC104 的 IV(0x80) 与 NT(0x40) 是两件事，压成 bool 必然选一个更差的表达 ——
压向 IV 会让主站把 5 秒前的真实值显示成"没数据"，压向 GOOD 会让主站拿陈旧值当实时值。
而这条信息在 RT_DB 源头本来就存在（`QUALITY_UNCERTAIN`），不该在转发层丢掉。

**端到端实测基线**（三进程 + 主站模拟器，两阶段双侧证据）：

| 阶段 | 设备进程 | 品质位 | 值 |
| --- | --- | --- | --- |
| A | 运行中 | 全 `0x00` GOOD（仅 `CMD.*` 三点 NT，因 EMS 未跑） | 实时变化 |
| B | 已终止 + 等 5 s | 全 `0x40` NT | 冻结在最后一次好值 |

**为什么这件事值得单列一节**：本模块修掉的 6 个缺陷**全部是"静默出错"型** ——
其中最贵的一个是 RT_DB 品质常量镜像写反（`kGood` 抄成 `0`）：值全对、
协议层无异常、单元测试全绿，只是调度画面上 24 个点**全灰**。
它只能靠**端到端的外部真值对账**发现。完整复盘见
[`P3/docs/README.md`](./P3/docs/README.md) §7。

> **GPL-3.0 提示**：`vendor/lib60870` 是 GPL-3.0，本项目静态链接它。
> 这通常意味着整个 EMS 二进制要按 GPL 发布。**本轮不做决定，需商务/法务确认**，
> 三条可选路线见 [`P3/docs/README.md`](./P3/docs/README.md) §6.4。


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
   ┌─────────┬──────────────┬──────────────┬─────────────┐
   │ Sim     │ Memory       │ RT_DB        │ IEC104      │  ← 适配器可换，
   │ DeviceIO│ DeviceIO     │ RtDbDeviceIO │ (P3 ✅)     │     算法零改动
   │ (P0)    │ (P0.5)       │ (已接入 ✅)  │             │
   └─────────┴──────────────┴──────────────┴─────────────┘
                                                    ▲
                                    注意：IEC104 是**对外**协议，
                                    不是又一种设备 I/O —— 它是
                                    站在 RT_DB 外侧的只读网关（P3/）
```

> 一句话区分 02/ 与 05/：**02/ 是设备侧安全处理器，05/ 是系统级安全边界统一器** —— 前者回答"这台设备能不能动"，后者回答"整站这一拍最终允许多少功率"。

### 模块依赖方向（头文件层，无环）

```
  04/ ──► 05/ ──► 06/ ──┐
    │                   ├──► 07/ ──► 08/ ──► 09/ ──► 10/ ──► 11/ ──► 12/
    └───────────────────┴──────────────┘
                              ├──► P1/（产品化配置化：装配全栈 + 配置驱动）
                              └──► P2/（产品化可观测性：外挂观察者，不修改 07/）
        04/ 被所有模块复用（策略基类 / 数据模型 / 仲裁器 / **device_io**）
        09/ 是**验证层**（周期 9）：装配 04~08 全栈跑 12000 拍闭环时序
        10/ 是**装配层**（周期 10）：把全栈装进可配置的 24h 场景，产出交付物
        11/ 是**联调层**（周期 11）：把全栈拆成两个**进程**，中间只走 RT_DB 共享内存
        12/ 是**验收层**（周期 12）：对上游**纯消费**，不新增算法、不修改任何模块
        P1/ 是**产品化配置层**：把"装配顺序知识"从调用点收进 apply_config()
        P2/ 是**产品化可观测层**：只读 StepRecord + rt 公开状态，obs.on_step() 是唯一接入点
        P3/ 是**产品化对外协议层**：只读 RT_DB，对上开 IEC104 从站；07/ 及以下不知道它存在
        vendor/ 是**第三方库快照**：只增不改，上游与 P3/ 的改动都不往里面写
        09/ 10/ 11/ 12/ P3/ 都不产出被其他模块依赖的头文件
```

| 模块 | 头文件搜索路径 | 说明 |
| --- | --- | --- |
| `05/` | `src` `../04/src` | 测试另需 `../06/src`（T03 交叉验证"矛盾→DERATED 仍允许输出"） |
| `06/` | `src` `../04/src` `../05/src` | 只用 `SafetyVerdict`，不依赖 07/ |
| `07/` | `src` `../04/src` `../05/src` `../06/src` `../08/src` | `EmsRuntime` 装配 08/ 的协同层；P0 新增 `device_io.h`；**RT_DB 接入测试另需 `src/rtdb` `vendor/rt_db`（并链接 `rt_db_api.c` / `ems_point_table.c` / `ems_rt_db_setup.c` 三个 C 文件）** |
| `08/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` | 演示与 T20 用 07/ 做端到端 |
| `09/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` | 只用 07/ 的 `EmsRuntime`，不反向被依赖 |
| `10/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` | 只用 07/ 的 `EmsRuntime`；曲线复用 08/ 的 `ForecastSeries` |
| `11/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` `../P2/src` + `src/rtdb` `vendor/rt_db` | 联调层：装配全栈为 `EmsSideApp`，设备侧为 `DeviceSideSim`；链接 `rt_db_api.c` / `ems_point_table.c` / `ems_rt_db_setup.c` |
| `12/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` `../09/src` `../10/src` `../11/src` `../P1/src` `../P2/src` + `src/rtdb` `vendor/rt_db` | 验收层：唯一**同时**依赖全部上游模块的模块（这是"最终验收"的定义决定的） |
| `P1/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` | 配置化装配层：绑定 04~08 的参数结构体 + 07/ 的 `EmsRuntime`；自带 JSON 解析器，零第三方依赖 |
| `P2/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` `../10/src` | 可观测层：只读 `StepRecord` + `rt.fsm().history()` + `rt` 公开状态；**不修改 07/**；测试侧用 `10/` 做交叉校验 |
| `P3/` | `src` `../07/src/rtdb` `../07/vendor/rt_db` + `../vendor/lib60870/{config,src/inc/api,src/inc/internal,src/common/inc,src/hal/inc}` | 对外协议层：**只读** RT_DB；链接 `lib60870.a` + `rt_db_api.o` / `ems_point_table.o` / `ems_rt_db_setup.o`。测试只需 `src` + `../07/src/rtdb` + `lib60870.a` + `ems_point_table.o`（用 `MemorySource`，不碰共享内存） |
| `13/` | `src` `../07/src/rtdb` `../04/src` | 设备接入层：**跨网络**（Modbus TCP **主站**）。链接 `ems_point_table.o` + `-lws2_32`；**不需要** RT_DB 共享内存，也不需要 lib60870。点表点名与索引直接取自 `07/src/rtdb/ems_point_table.h`（不另抄一份）。跨语言测试另需外部 Python + pymodbus（缺失时该层 SKIP，不影响其余） |
| `vendor/lib60870/` | 自含（`config` `src/inc/api` `src/inc/internal` `src/common/inc` `src/hal/inc` `src/file-service`） | C99 静态库。**必须 `gcc -std=gnu99`**（不是 g++）；链接需 `-lws2_32 -liphlpapi -lbcrypt`。上游源码一行未改 |

> **一个反直觉的 include 顺序约束**：`07/vendor/rt_db/rt_db_structs.h` 定义了
> `atomic_store_explicit` / `atomic_thread_fence` 这类**函数式宏**，会破坏 libstdc++
> 里同名 `std::` 函数的**声明**（错误却报在该头第 34 行）。所以 `P3/src/rtdb_quality.h`
> **必须排在所有 C++ 标准库头之前** include；它内部会立刻擦掉这些宏，并带一道 `#error`
> 前置检查。同一个头还负责用 `static_assert` 把 RT_DB 的品质编码钉在真相源上 ——
> 手抄常量已经出过一次"24 个点全灰"的事故，见 [`P3/docs/README.md`](./P3/docs/README.md) §7.1。

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
- **系统级联调：三进程 + 共享内存（周期 11）** → [`11/docs/README.md`](./11/docs/README.md)（§5 **四个**真实缺陷复盘、§7.1 L2 极限环遗留）
- **最终验收：七维度交付门禁（周期 12）** → [`12/docs/README.md`](./12/docs/README.md)、[`12/docs/design.md`](./12/docs/design.md)
- **模拟器与设备接入梳理（四个接口方向 / 两层点表 / 指令下行两条路径 / 缺口清单）** → [`docs/规划/模拟器与设备接入梳理.md`](./docs/规划/模拟器与设备接入梳理.md)（**§10 = 仍未做好的地方**；**A1 / A2 / B1 / A3.1 已交付**，A3.2 与 B/C/D 类仍未做）
- **产品化 P0：算法 ↔ 设备解耦（IDeviceIO / SimDeviceIO / MemoryDeviceIO）** → [`docs/产品化/P0-架构分层.md`](./docs/产品化/P0-架构分层.md)
- **产品化 P1：配置化（字段绑定表 / 装配顺序 / 校验 / 模板与文档生成）** → [`P1/docs/README.md`](./P1/docs/README.md)
- **产品化 P2：可观测性（边沿检测→SOE / 时间窗抑制 / O(1) 增量指标 / 指标与 log_every 解耦 / 三个正交旋钮）** → [`P2/docs/README.md`](./P2/docs/README.md)
- **产品化 P3：IEC104 从站网关（点表真相源 / 品质三态 / ASDU 分包 / 现场部署 / 6 个缺陷复盘）** → [`P3/docs/README.md`](./P3/docs/README.md)、[`P3/docs/design.md`](./P3/docs/design.md)
- **lib60870 第三方库接入（来源 / commit / 编译参数 / 两个坑 / GPL-3.0 授权说明）** → [`vendor/lib60870/README.md`](./vendor/lib60870/README.md)
- **RT_DB 接入：共享内存实时库适配器（跨内存边界闭环等价 / 点表契约 / 双连接 / Windows 存活句柄）** → [`07/docs/README.md`](./07/docs/README.md) §8
- **多策略协同的接口约定** → [`docs/接口规范/EMS策略接口规范.md`](./docs/接口规范/EMS策略接口规范.md)（§2.5 仲裁算法即 04/strategy_arbiter.h 的实现依据）

---

## 5. 代码组织原则

| 原则 | 说明 |
|---|---|
| **src/tests 分层** | 每个模块内部按 `src/` 源代码、`tests/` 测试、`samples/` 示例、`data/` 仿真 CSV、`figs/` 图、`docs/` 文档、`scripts/` 构建运行脚本、`build/` 产物分开。 |
| **共享代码唯一副本** | `03/shared/clamper.h` 是三个控制器共用 clamp，原本每处一份（共三份 + `ar_clamp` 模板），现在统一为 `bms::clamp`。 |
| **集成项目不复制源码** | `03/integration/src/IntegrationMain.cpp` 通过 `-I` 引用三个子项目的 header，源码不复制（原本有 6 份 `*.h/*.cpp` 重复，已删除）。 |
| **构建可重现** | 每个模块一个 `scripts/build.bat`；顶层 `scripts/build_all.bat` 串联全部。修改任何模块后只需重跑所在模块的 build.bat。 |
