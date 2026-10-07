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
│   ├── src/record_csv.h                      ← ★ **实录 CSV 契约（20 列）**：实时源与现场进程共用同一份格式
│   ├── src/field_args.h                      ← 现场进程入口的参数解析（--device / --conn / --record 优先级）
│   ├── src/main_field.cpp                    ← **现场进程入口** main_field.exe（sim / rtdb / modbus 三数据源，默认只读）
│   ├── src/demo_scenario7.h                  ← 现场演示用故障剧本（供 --device sim 跑）
│   ├── vendor/rt_db/                         ← RT_DB 源码快照（api .c/.h + structs + private）
│   ├── tests/test_realtime_loop.cpp          ← T11~T16（6050 断言）
│   ├── tests/test_device_io.cpp              ← T21~T24（81 断言，P0/P0.5 适配器可换性）
│   ├── tests/test_rtdb_device_io.cpp         ← T25~T29（554 断言，RT_DB 接入：跨内存边界闭环等价）
│   ├── tests/test_field_entry.cpp            ← T-F1~T-F9（124 断言，现场入口参数解析与优先级）
│   ├── docs/                                 ← README.md + design.md
│   ├── scripts/                              ← build.bat / build_test*.bat / build_field.bat / run_demo.bat
│   └── build/                                ← loop_demo.exe + test_*.exe + **main_field.exe**
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
│   ├── src/sim_live.h                         ← ★ **实时仿真源**：按墙钟 `sleep_until` 逐拍推进，写 07/ 同款实录 CSV
│   ├── src/main_live.cpp                      ← sim_live.exe（运行模式的生产数据源；`--duration-s 0` = 一直跑到被停）
│   ├── tests/test_sim_24h.cpp                ← T101~T113（166 断言）
│   ├── data/typical_day_96.csv               ← 典型日曲线（96 点 × 15 min）
│   ├── scripts/gen_curves.py                 ← 曲线生成器
│   ├── docs/README.md                        ← 经济性口径 / 缺陷复盘 / 故障注入语义 / 实时源与多日仿真
│   └── build/                                ← sim_demo.exe + **sim_live.exe** + test_sim_24h.exe
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
    ├── src/device_conn_conf.h                ← ★ 接入参数（配置通道②）：active.conn 解析与默认路径
    ├── src/main_probe.cpp                    ← 现场点表核对工具 modbus_probe.exe（--load-map / --dump-map / --dump-conn）
    ├── tests/test_modbus_tcp.cpp             ← T01~T38 协议层 + 运行期点表 + 两条配置通道（597 断言，不需要 Python）
    ├── tests/test_modbus_device_io.cpp       ← T21~T30 适配器契约（133 断言，不需要 Python）
    ├── tests/test_modbus_bridge.cpp          ← T40~T47 跨语言联调（83 断言，**需 pymodbus**）
    ├── sim/modbus_slave.py                   ← Python 从站（pymodbus）+ 设备模型 + 32 点 TSV 导出
    ├── scripts/                              ← build.bat / build_test.bat / run_sim.bat
    └── docs/                                 ← README.md（怎么跑 / 三层测试 / 6 个缺陷复盘）+ design.md + point_map_template.csv

14/                                           ← 平台层后端 + SQLite 数据库（**框架第四层：平台层**）
    ├── schema.sql                            ← 18 张表（§22 六张表 + 时序明细/场景/经济性/不变量/用户/会话/审计 + 运行模式 + 设备接入两张 + 仿真台账），schema v1.4
    ├── src/emsdb.py                          ← 连接 / 建表 / 通用查询（WAL：导入不挡读；后台线程靠 `Conn.db_path` 继承库路径）
    ├── src/importer.py                       ← 把 10/build 的**真实仿真产物**装进库
    ├── src/auth.py                           ← 口令（pbkdf2+盐）/ 会话（token）/ 三角色 / 审计
    ├── src/engine.py                         ← **运行模式实时引擎**：尾随实录 CSV 增量入库 / 停顿判定 / 重启接活
    ├── src/simrun.py                         ← **日 / 周 / 月批量仿真调度** + 台账（进度轮询 / 删除连带清 6 张表）
    ├── src/api.py                            ← REST 路由与业务逻辑（41 个端点）
    ├── src/server.py                         ← HTTP 服务（标准库，同时托管 15/ 静态页）
    ├── tests/selftest.py                     ← 十段自检（511 断言；含实时入库 [I] 段、引擎双模式 [J] 段）
    ├── scripts/                              ← import_sim.bat / import_live.bat / run_server.bat / run_selftest.bat / run_all.bat
    └── docs/README.md                        ← 为什么需要 / 表与来源 / 41 个接口 / 权限 / 已知边界

15/                                           ← 平台层前端 + 运行界面（**框架第四层：平台层**）
    ├── index.html                            ← 登录视图 + 主界面骨架（脚本引用顺序即依赖顺序）
    ├── assets/app.css                        ← 浅色 SaaS 主题（四个量测色：负荷/光伏/电网/电池）
    ├── assets/api.js                         ← REST 客户端（唯一碰 fetch 的地方；401 统一重登录）
    ├── assets/ui.js                          ← DOM 辅助 + **手写 SVG** 折线/柱状/能量流（不引图表库）
    ├── assets/pages.js                       ← 10 个页面（总览/实时/曲线/告警/策略/收益/报表/仿真/设备/用户）
    ├── assets/app.js                         ← hash 路由 + 登录流 + **引擎/场景双切换** + 菜单角色过滤 + 运行模式 5 s 自刷
    ├── tests/selftest.js                     ← 静态契约自检（205 断言，Node）
    ├── scripts/                              ← run_ui.bat / run_selftest.bat / run_all.bat
    └── docs/README.md                        ← 10 页结构 / 接口对齐 / 权限 / 已知边界

16/                                           ← 通信加固（**上现场前必须补的工程欠项**，6/7 条）
    ├── src/backoff.h                         ← 重连指数退避 + 抖动（确定性 PRNG）
    ├── src/link_arbiter.h                    ← 主备双链路仲裁（N 切备 / M 切回，滞环防抖）
    ├── src/sntp.h                            ← SNTP 对时（1900↔1970 纪元、32.32 定点、回拨钳制）
    ├── src/modbus_rtu.h                      ← Modbus RTU（CRC16 + 3.5T 分帧 + 假串口）
    ├── src/ftp.h                             ← FTP 定值下发通道（PASV / 多行应答 / 150-226）
    ├── src/tls.h                             ← TLS：**Windows SChannel 真实现**；非 Windows 显式拒绝
    ├── tests/*.cpp                           ← 五层 T01~T89（3898 断言；TLS 层需 python 对端）
    ├── scripts/                              ← build.bat / build_test.bat
    └── docs/README.md                        ← 6 条欠项的选型理由 / 8 个坑 / TLS 现状与接入步骤

17/                                           ← 设备全点表与高保真模拟器（缺口清单 B 类 + C 类）
    ├── src/device_point_table.h              ← 设备侧**全点表**（BMS / 电表 / PCS），与 07\ 的 40 点抽象表分层
    ├── src/point_mapping.h                   ← 两层点表映射（**是代码不是文档**：直接/换算/合成/派生）
    ├── src/point_config.h                    ← 点表配置化（JSON 往返幂等；**换站改配置不改源码**）
    ├── src/meter_sim.h / bms_sim.h / pcs_sim.h  ← 三个高保真模拟器（替换"两个 sin()"）
    ├── src/deadband.h / tiered_scheduler.h / publish_pipeline.h  ← C2 死区 / C3 分级节拍 / C1 批量写
    ├── tests/*.cpp                           ← 六组（721 断言）
    ├── scripts/                              ← build.bat / build_test.bat
    └── docs/README.md                        ← C1/C2/C3 量化结果（API 调用 61200 → 600）/ 11 个坑

18/                                           ← 性能基准与压测（SLA 硬门禁）★ 时序敏感，勿并发构建
    ├── src/perf_clock.h / perf_stats.h       ← 计时（PIT 而非假精度的 steady_clock）/ 分位数（nearest-rank 显式）
    ├── src/bench_*.h                         ← 单拍闭环 / 采集 / Modbus 编解码 / SOE 告警 / 发布（全量 vs 优化）
    ├── src/bench_soak.h                      ← 24h 长跑（头尾自比，看性能是否随运行时长退化）
    ├── src/sla.h                             ← 23 条 SLA：每条标来源（推导 / 实测裕度 / 继承）+ 反向验证
    ├── tests/*.cpp                           ← 91 断言
    ├── build/BENCH-REPORT.md                 ← **每次运行自动生成**的基准报告（不入库）
    ├── scripts/                              ← build.bat / build_test.bat
    └── docs/README.md                        ← 数字可信边界 / 已知边界 / 8 个坑

19/                                           ← 现场部署 P5（守护 / 配置版本 / 日志轮转 / 作业指导书）
    ├── src/ems_supervisor.py                  ← 按 11\ 的硬依赖顺序拉起三进程 + 存活检测 + 退避 + 级联重启
    ├── src/config_store.py                    ← 配置快照 / diff / **回滚（回滚前自动再存一版）**
    ├── src/log_rotator.py                     ← SOE + metrics 落盘与轮转（大小 + 条数双阈值，不丢事件）
    ├── tests/*.py + harness.py                ← 四套（424 断言）
    ├── scripts/                               ← install/uninstall ×（task | service）+ run_supervisor.bat
    ├── docs/现场作业指导书.md                  ← ★ 从空服务器到 EMS 在跑的可复现步骤（含点表逐点核对清单）
    └── docs/README.md                         ← 为什么选计划任务而不是 Windows 服务 / 11 个坑

20/                                           ← 告警能力独立 + SOE 持久化 + 配置持久化（清三条技术债）
    ├── src/alarm_model.h / alarm_input.h / alarm_assembler.h  ← 与仿真无关的告警装配（SET/CLEAR 成对）
    ├── src/soe_store.h + soe_dump.cpp         ← SOE 落盘（文件是唯一真相源，一条事件一行）
    ├── src/config_store.h                     ← 配置落盘 + 版本（缺字段必须**指名报错**，不许默认值）
    ├── src/alarm_demo.cpp                     ← 告警 + 持久化端到端演示
    ├── tests/*.cpp                            ← 三个（397 断言）
    ├── scripts/                               ← build.bat / build_test.bat
    └── docs/README.md                         ← 六类故障源与 11\§4 的逐条对齐表 / 容量算式 / 6 个坑
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
07\build\test_device_io.exe                 产品化 P0/P0.5 适配器可换性（T21~T24 / 81 断言）
07\build\test_rtdb_device_io.exe            RT_DB 接入（T25~T29 / 554 断言，共享内存实时库）
07\build\main_field.exe                     07/ 现场进程入口（--device sim|rtdb|modbus，默认 sim；默认只读）
07\build\test_field_entry.exe               07/ 现场入口参数解析（T-F1~T-F9 / 124 断言）
08\build\coord_demo.exe                     周期 8 场景 D：24h 分层协同
08\build\test_dispatch_coordinator.exe      08/ 单元测试（T17~T20 / 444 断言）
09\build\test_multi_strategy.exe            09/ 单元测试（T91~T97 / 77 断言；7 场景 × 12000 拍闭环）
10\build\sim_demo.exe                        周期 10 场景 E：EMS 24h 离线仿真（产物 report.html 等 4 个文件）
10\build\sim_live.exe                        **实时仿真源**：按墙钟逐拍跑并写实录 CSV（运行模式的生产数据源）
10\build\test_sim_24h.exe                    10/ 单元测试（T101~T113 / 166 断言）
11\build\rtdb_initializer.exe                 周期 11 共享内存段初始化器（段 + 全点表注册，常驻保活）
11\build\device_side.exe                      周期 11 设备侧进程（六类数据源 + 故障剧本）
11\build\ems_side.exe                         周期 11 EMS 侧进程（全栈闭环，消费共享内存）
11\build\test_system_integration.exe         11/ 单元测试（T41~T49 / 152 断言）
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
13\build\test_modbus_tcp.exe                  13/ 协议层 + 两条配置通道 T01~T38（597 断言；不需要 Python）
13\build\test_modbus_device_io.exe            13/ 适配器契约 T21~T30（133 断言；不需要 Python）
13\build\test_modbus_bridge.exe               13/ 跨语言联调 T40~T47（83 断言；**需 Python + pymodbus**）
13\sim\link_tap.py                             13/ **链路中间人**（报文监视 + 链路扰动 + 测点注入；两端零改动）
13\tests\test_link_tap.py                      13/ 中间人自证 L01~L12（53 断言；无外部依赖）
13\tests\e2e_link_tap_live.py                  13/ 真实链路端到端（29 断言；需 pymodbus + modbus_probe.exe）
vendor\lib60870\build\lib60870.a              IEC 60870-5-104 协议栈静态库（gcc -std=gnu99）
14\src\server.py                              平台层后端服务（标准库 HTTP；同时托管 15\ 静态界面）
14\src\pointmap.py                             点表校验/序列化/落盘（规则镜像 13/ validate_map）
14\src\connconf.py                             接入参数序列化/落盘（配置通道②；规则镜像 13/ device_conn_conf）
14\scripts\import_live.bat                     实时入库（07/ --record 的 CSV → scenario.time_base='wall'）
15\index.html                                 平台层运行界面（零依赖零打包；图表为手写 SVG）
16\build\test_backoff_arbiter.exe             16/ 重连退避 + 主备双链路（T01~T20 / 3094 断言）
16\build\test_sntp.exe                        16/ SNTP 对时（T30~T40 / 193 断言）
16\build\test_modbus_rtu.exe                  16/ Modbus RTU 串口（T50~T65 / 310 断言）
16\build\test_ftp.exe                         16/ FTP 定值下发（T70~T78 / 175 断言）
16\build\test_tls.exe                         16/ TLS 端到端（T80~T89 / 126 断言；**对端是 Python ssl**）
17\build\test_point_table.exe                 17/ 两层点表结构（C4/B2 / 110 断言）
17\build\test_point_config.exe                17/ 点表配置化 JSON 往返（C5 / 160 断言）
17\build\test_sim_meter.exe                   17/ 关口电表模拟器（B4 / 63 断言）
17\build\test_sim_bms.exe                     17/ BMS 模拟器（B5 / 113 断言）
17\build\test_sim_pcs.exe                     17/ PCS 模拟器（B3 / 73 断言）
17\build\test_scale_rtdb.exe                  17/ 分级节拍 + 死区 + 批量写（C1/C2/C3 / 202 断言）
18\build\test_perf_stats.exe                  18/ 计时与统计基础设施（T01~T11 / 63 断言）
18\build\test_perf_bench.exe                  18/ 基准 + 23 条 SLA 门禁 + 反向验证（B1~B6 / 28 断言）
18\build\BENCH-REPORT.md                      18/ **每次运行自动生成**的基准报告（数字 / SLA 对照 / 反向验证）
19\src\ems_supervisor.py                      19/ 进程守护器（按 11\ 的硬依赖顺序拉起三进程）
19\src\config_store.py                        19/ 配置版本：snapshot / list / diff / rollback
19\src\log_rotator.py                         19/ SOE 与 metrics 落盘 + 轮转
20\build\soe_dump.exe                         20/ SOE 落盘文件读取 / 过滤 / 导出（--dump）
20\build\alarm_demo.exe                       20/ 告警能力 + 持久化端到端演示
```

**全量回归：16178 断言全绿（`exit=0`）**—— 这是**本机口径**：`python` + `pymodbus` + Node
齐备（`13/` 下建过 `.venv`，`build_test.bat` 会自动捡起它）。四个合法口径见下表，别混读。
`02/` 与 `03/` 只印 `ALL TESTS PASSED` 不印计数，故不入明细；其余 **23 项**加总如下
（按**齐备口径**列，其中 `13/` 齐备为 **813**；缺 pymodbus 时实跑 **735**，
故缺 pymodbus 合计 = 16178 − 78 = **16100**）：

```
04=94   05=94   06=34   07=6050  P0+P0.5=81  RT_DB=554  07-field=124  08=444
09=77   10=166  P1=171  P2=434   11=152      12=114     P3=529（协议 408 + EXT 落点 121）
13=813（缺 pymodbus 时 735）     14=511      15=205
16=3898（缺 python 时 3840）     17=721      18=91      19=424   20=397
```

`[BUILD ALL OK] All 41 components built.`

> **不要再手工加总** —— 用 `python scripts/baseline.py` 从 `build_all.log` 直接算。
> 它逐行解**混合编码**（见下文告警），并按「每步取汇总行；没有汇总行就加总各 exe」
> 的口径列成 **23 项**明细，顺带校验步骤号 1..N 无缺号（与 `12/` 验收器 A7-04 同一条纪律）。
> 本机实跑：`{"steps": 41, "denom": 41, "counted": 23, "total": 16178, "consistent": true}`。

> **★ 两处环境依赖，让基线有四个合法值**（写报告时必须说明用的哪个口径）：
>
> | 条件 | 少了什么 | 全量基线 | 日志里的正面证据 |
> | --- | --- | --- | --- |
> | `python` + `pymodbus` + Node 都齐备 | — | **16178** ← **本机实测** | `SKIPPED=0` + `[OK] 15/ 静态契约自检通过` + `[OK] 16/ 通信加固 五层测试全部通过` |
> | 缺 `pymodbus` 的 Python | `13/` 跨语言层 **−78**（83 条里 5 条仍实跑，见下） | **16100** | `[SKIP] … 跨语言层未运行` |
> | 缺 Node | `15/` 静态契约自检 = 205 | **15973** | `[SKIP] 未找到 Node 运行时` |
> | 两者都缺（**开箱默认**） | 78 + 205 = 283 | **15895** | 上面两行同时出现 |
>
> ★ **缺 pymodbus 时不是 −83，而是 −78**（实测修正）。
> 跨语言层 8 个用例里 **T40 点表一致性走 `--dump-tsv`，不需要 pymodbus**，因此照常实跑。
> 实测（`EMS_PYTHON` 指向不带 pymodbus 的解释器）：
> `13/build/bridge_status.txt` = `PASS=5 FAIL=0 SKIPPED=7`、`13/` 合计 **735**（597+133+**5**）。
> 而 `scripts/baseline.py` 对 `13/` 这一"无汇总行"步骤是按**各 exe 相加**取值的
> （`step_count()` 的最后一支），所以全量基线也随之 **−78**。
> —— `SKIPPED=7` 数的是**用例数**，与断言差值 **78** 不是同一个单位，别混读。
>
> **★ 还有第三个环境开关，但它不构成第五个口径**：`16/` 的 TLS 层需要 `python`
> （`EMS_PYTHON` 或 PATH 上的 `python.exe`）。缺它时该层 6 条显式 SKIP ⇒
> `16/` 从 **3898 降为 3840**（−58）。
> 但这种情况**不会单独出现** —— `14/` 与 `19/` 本身就是 Python 模块，没有 python
> 全量构建**根本跑不完**（`19/scripts/build_test.bat` 会印 `[FAIL] no usable python`）。
> 所以「缺 python」不是一个口径，而是一个**没跑完的构建**，按失败处理。
>
> **判据**（推荐直接用 `python scripts/baseline.py`，它一次把口径和明细都印出来）：
> `grep -c "SKIPPED=0" build_all.log`（`13/`、`16/`）与
> `grep -c "静态契约自检通过" build_all.log`（`15/`）—— **两项都要看**，
> 只看一项会把另一处的环境缺失误判成"完整口径"。
> ⚠️ 加了 `16/` 之后 `grep -c "SKIPPED=0"` 会数到 **多处**（16/ 逐层都印），
> 所以**不要再拿这个 count 当布尔判据**，改用 `baseline.py` 的 `--json` 输出。
>
> 三个模块都**总是打印状态行**（哪怕是 0 条跳过）—— 只在缺失时才打印的话，
> "确实跑了"在日志里就**没有正面证据**，只能靠"没有那一行"去推断，
> 而"缺行"与"grep 写错 / 日志被截断"完全无法区分。
>
> 跨语言层需要**外部 Python + pymodbus**，环境缺失时以 0 退出（环境问题不是代码缺陷），
> 但 `build_test.bat` 的收尾行会**据实**改印 `[SKIP]`，不会仍然声称"三层全部通过"。
> 建环境：`python -m venv 13\.venv` + `pip install -r 13\sim\requirements.txt`，
> 然后设 `EMS_PYTHON`（`13/scripts/build_test.bat` 也会自动捡起 `13\.venv`）。
> 同理，`16/` 的 TLS 层缺 python 时**不会**印"五层全部通过"，而是印 `[SKIP]`
> 并回显"TLS 端到端断言未验证"。
>
> **★ `15/` 的自检需要 Node**（`EMS_NODE` 或 PATH 上的 `node`）。
> 15/ 是纯静态前端、没有编译步骤，进全量回归的就是它的**静态契约自检**
> （205 条：文件结构 / 接口契约 / 页面与权限 / 渲染纪律 / 图表齐备 / 口径一致 / 模式与接入参数 / **引擎双模式**）。
> 定位不到 Node 时打印 `[SKIP]` 并以 0 退出 —— 环境缺失不是代码缺陷，
> 但**必须显式喊出来**（同 `13/` `16/` 的处理），否则"静默跳过"与"真的跑过"不可区分。

> **断基数口径**：**23 项**明细加总（齐备口径）= 16178；缺 pymodbus = **16100**。
> 三种**打印格式不同**，扫描时要认全：
>
> | 格式 | 谁 |
> | --- | --- |
> | 不印计数（只印 `ALL TESTS PASSED`） | `01/ 03/` 各步、**`02/`**、`04/` 的 demo 步 |
> | 中文格式 `通过 N / 失败 M` | **`P1/`**（171）、**`P2/`**（434）、**`15/`**（205） |
> | `PASS=N FAIL=M` / `PASS=N` | 其余大部分（注意**有的只印 PASS 不印 FAIL**） |
> | `PASS=n  FAIL=m  SKIPPED=k`（三栏） | **`14/`**（511；即使全跑也会印出 `SKIPPED=0`）、**`19/`**（424） |
> | **带前缀的汇总行** `合计 PASS=…` | **`16/`**（3898，五层逐层都印 + 末行汇总） |
> | **带前缀的汇总行** `assertions total: PASS=…` | **`17/`**（721）、**`19/`**（424，与上一行同一行） |
> | **不打汇总行**，逐 exe 各印一行 | **`20/`**（176 + 144 + 77）、**`P3/`**（408 + 121）、**`13/`**（597 + 133 + 83） |
>
> ⚠️ 最后一行是**最容易算错**的一类：那三步的末行只是**最后一个 exe** 的数
> （`20/` 会读成 77、`P3/` 读成 121、`13/` 读成 83），必须**加总**才对
> （397 / 529 / 813）。`scripts/baseline.py` 专门处理这一类。
>
> 只 `grep PASS=` 会漏掉 P1/P2/15 的 **764** 条。`02/` 在明细里写成 `02 /`（不带 `=n`）。
> **`07/` 现场入口（`test_field_entry`，124）是独立一步**（步骤 21），
> 与 `07/` 主测试（6050）、P0+P0.5（81）、RT_DB（554）**分开计** —— 别把它并进 6050。
>
> **★ 日志是混合编码的**：`build_all.log` 里 bat 的 `echo` 是 **CP936**，
> 而 `P1/P2`（自研 C++ 打印）与 `15/`（Node）的 stdout 是 **UTF-8** ——
> 两者混在同一个文件里。**整文件按单一编码解码必然漏掉另一半**：
> 实测按 CP936 解码时 `15/` 的「通过 125」变成乱码 `閫氳繃 125`，
> 正则匹配不上，基线就这么少算了 125。
> 正确做法是**逐行**先试 UTF-8、失败再退 CP936（或反之），
> 而不是整文件 decode 一次。`scripts/baseline.py` 已按这条实现，**直接用它**。
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
> **其余不动**）→
> **2026-09-25 新增 `14/` 平台层后 9503 / 9581**（新模块 **199**，其余不动。
> 平台层是 Python 标准库实现，不进 C/C++ 编译链，但**同样进全量回归**）→
> **2026-09-25 合并远程 `11/ 12/ 13/ vendor/` 分支 + 新增 `15/` 前端后 9745**
> （完整环境口径）→
> **2026-09-25 修掉电量放大 8640 倍 + 跨日重导堆积后 9748**
> （`14/` 199 → **209**（+10，电量护栏）→ **212**（+3，跨日重导护栏）；
> 其余不动。详见 `CHANGES.md` §39）→
> **2026-09-26 补齐工程欠项与缺口清单后 15279**（新增五个模块：
> `16/` 通信加固 **3898** / `17/` 全点表与模拟器 **721** / `18/` 性能基准 **91** /
> `19/` 现场部署 **424** / `20/` 告警与持久化 **397**；合计 **+5531**，其余不动。
> 构建入口 33 → **39 步**。详见 `CHANGES.md` §40~§45）→
> **2026-09-27 运行模式 + 两处界面修复后**（`14/` 212 → **235**（+22 运行模式接口测试、
> +1 表数期望 14→15）；`14/` overview 新增 `grid_limit_kw`，`15/` 关口卡对变压器容量判越限、
> 能量流箭头随充放翻转。`15/` 仍 125）→
> **2026-09-28 现场进程入口 + 实时入库 + 配置通道后**（`14/` 235 → **348**（+113，
> 实时入库 [I] 段 14 条 + 运行模式类）；`07/` **新增「现场入口」一步**（步骤 21，
> `test_field_entry` = **111**；T-F1~T-F8 = 100 → T-F9 = **111**，+11）；
> `13/` 协议层 489 → **513**（+24，T16~T20 → T16~T33）；
> `18/` **91**、`19/` **424** 各因本回合的两个**时序缺陷修复**恢复全绿
> （见下方「两个时序缺陷」）；构建入口 39 → **41 步**。详见 `CHANGES.md` §52~§53）→
> **2026-09-29 配置通道②（接入参数）全链路后**（`13/` 协议层 513 → **597**（+84，
> 新增 T34~T38：接入参数端侧解析 + 同目录判据）；`14/` 348 → **389**（+41，接入参数
> 落盘 / 同事务回滚 / 跨语言判据 G12~G13）；`15/` 125 → **159**（+34，新增接入参数
> 下发 H 段等）；`07/` 现场入口 111 → **124**（+13）；`10/` 144 → **142**（−2，
> 系 09-28 下午 `sim_24h.h / sim_report.h / test_sim_24h.cpp` 的未提交改动所致）。
> 详见 `CHANGES.md` §54）→
> **2026-10-07 运行 / 仿真双模式后 16178**（`10/` 142 → **166**（+24，T112/T113：
> 实时源与离线仿真的**逐列等价断言** + 时长与护栏）；`14/` 389 → **511**（+122，
> 新增整个 [J] 段引擎双模式 + [F] 段引擎接口断言，表数 17 → 18、schema v1.3 → **v1.4**）；
> `15/` 159 → **205**（+46，新增 [I] 段引擎双模式契约，菜单 9 → 10 项含「仿真」页）；
> **其余不动**。详见 `CHANGES.md` §56）
>
> ⚠️ **同一回合修掉的一次真红**：首轮全量构建 `19/` 从 424 → **423**，唯一红项是
> `[D] 反向守卫：ems 先成功起来过`。**不是守护器逻辑错**，也不是偶发（单跑 3/3 复现）：
> 该用例是**三跳链** `init → dev → ems`，而本机 Python 解释器冷启动已从文档记的
> 0.8 s 涨到 **1.42 ~ 1.85 s**，三跳 ≈ 6.7 s，可 `init` 当时只活 2.5 s（≈ 3.9 s 退出），
> `ems` 还没 ready 就被级联 kill。按 冷启动上限 2.0 s × 跳数 重标定
> （`init` 活 2.5 → 8.0 s、窗口 8.0 → 20.0 s）后 `PASS=73 FAIL=0`，`19/` 回到 424。
> 教训见 `19/docs/README.md` 坑 11c：**链上每多一跳就多乘一次冷启动**，
> 而"冷启动"这个常数会随机器漂移，**必须定期重测**。
>
> ★ **本机口径也变了**：`13/.venv` 建好后 `build_test.bat` 会自动捡起它，
> 跨语言层（83 条）从 `SKIP` 变成实跑 —— 本机实测口径因此由「缺 pymodbus」
> 转为**齐备**。这不是断言新增，是**环境补齐**，别算进增量。
>
> ★ **本回合起基线一律以 `python scripts/baseline.py` 实测为准**（不再手工加总）。
> 实测（本机：`python` + `pymodbus` + Node 齐备）：
> `{"steps": 41, "denom": 41, "counted": 23, "total": 16178, "consistent": true}`
> → 四口径 **齐备 16178 / 缺 pymodbus 16100 / 缺 Node 15973 / 两者都缺 15895**。
>
> ⚠️ **历史手工数字的一次对账**：本回合重算时发现旧的四口径（15545/15467/15420/15342）
> 与其**自己的明细行加总（15568）差 23** —— 正是 `baseline.py` 文件头警告的
> "手工加总必错"。旧四口径**不再引用**，一律用上面这组实测值。
>
> ⚠️ **9503 / 9581 已作废**：那是**合并远程之前**写的，当时仓库里还没有
> `11/ 12/ 13/ vendor/`。合并后模块构成从 26 涨到 33 组件（再补 `16/`~`20/` 后为 **39**，
> 再加 `07/` 现场入口一步后为 **41 步**），
> `13/`（597+133+83）与 `11/`（152）、`12/`（114）都是新的，`P3/` 的测试组织也被远程改过
> （646 → 408 + 121）。
> 两个数字**不能再引用**；基线一律以本章节上面的**四口径表**为准
> （齐备 **16178** / 缺 pymodbus **16100** / 缺 Node **15973** / 两者都缺 **15895**）。

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

> **两个时序缺陷修复（2026-09-28，见 `CHANGES.md` §53）—— 都是"测试在测机器，不是测代码"**：
>
> 1. **`18/` SLA-L2 改用稳健统计量**。原判据「末 1000 拍 p99 / 头 1000 拍 p99 ≤ 2」
>    用单个 1000 样本窗的 p99，而它是**第 10 差**的样本 —— 一次 OS 调度抖动落进窗口
>    就能把它抬 3~5 倍。实测**同一二进制连跑 5 次**：head p99 在 5.5~39 µs、
>    tail p99 在 7.8~35 µs 之间跳，比值在 **0.20~3.42** 之间翻 —— 那测的是
>    "抖动恰好落在头窗还是尾窗"，与"性能有没有随时长退化"无关
>    （本机 32 逻辑核，但后台进程多，抖动不可避免）。
>    **修法**：切 **16 个等长子窗**，逐窗取 p99，比较「末 1/4 子窗 p99 的**中位数**」
>    与「首 1/4 子窗 p99 的中位数」。修后**正向 16 次连跑比值全在 0.43~1.75**；
>    **反向验证**（注入随时长线性增长的延迟）反而从「1.90x 无区分度」变成
>    **3.88x 稳定变红** —— **又稳又灵，不是靠放水通过**。
> 2. **`19/` supervisor 退出码用例窗口 1.6 s → 4.0 s**。该用例子进程 `alive=0.2 s`，
>    但**真 Python 冷启动实测 0.696~0.771 s**（均值 0.73 s）：首拍退出 ≈0.9 s、
>    退避 0.3 s 后二拍启动 ≈1.2 s，而窗口只有 1.6 s —— 满载下窗口先关，
>    `starts=1 → restarts=0 → rc=0`（期望 2）。守护器逻辑没错，是**测试窗口比冷启动还紧**。
>    修后连续 `PASS=73 FAIL=0`。

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


### 2.19 平台层：数据库 + 接口（`14/` 产出）

```bat
14\scripts\run_all.bat        :: 导入 10/build 的真实产物 + 自检（199 断言）
14\scripts\run_server.bat     :: 启服务 127.0.0.1:8765（默认账号 admin/admin123）
14\scripts\run_selftest.bat   :: 只跑自检
```

**`14/` 补的是框架第四层「平台层」。** 此前 `01`–`13` / `P1`–`P3` 把**设备层**与
**EMS 核心控制层**做透了，但**平台层是空的**：没有数据库、没有接口、没有界面 ——
运行人员要回答"昨天发生了什么"，只能去翻 8640 行 × 20 列的时序 CSV。

```
   10/ 的仿真产物 ──导入──► SQLite（14 张表）──REST──► 15/ 界面（9 页）
   （真实产物，非编造）         │
                        账号 / 会话 / 审计
```

三件事值得单列：

| 事项 | 说明 |
| --- | --- |
| **不假装下发成功** | 现场没有真实执行器时，`POST /api/control/command` 返回 `accepted:false` + `executor:"none"`，只记录意图并审计；**绝不会在界面上显示一个没发生的动作** |
| **数据可追溯** | 自检 `[C]` 段把入库值**逐项对照** `10/build/summary.json`（净收益 572.01 元、充电量 535.313 kWh 等），证明平台展示的是真产物而不是编造的数字 |
| **零第三方依赖** | 只用 Python 标准库（`sqlite3` + `http.server`）。现场部署不该先装一堆包 —— 与项目其余部分一致 |

**数据边界**：库里所有现场量来自**两个源** —— `10/` 的离线仿真（`time_base='sim'`，一天内秒）
与 `07/` 现场进程 `--record` 的实时实录（`time_base='wall'`，Unix 墙钟秒，经 `14/scripts/import_live.bat` 增量入库）。
平台的"实时" = 仿真时标 `t_s`；实时场景走墙钟口径，两套时间轴不可混算（`h_series` 按 `time_base` 分支）。
接上真设备后无需改表结构与接口 —— 与 `IDeviceIO` 上"换介质不改算法"是同一个设计。

详见 [`14/docs/README.md`](./14/docs/README.md)。

### 2.20 平台层：运行界面（`15/` 产出）

```bat
15\scripts\run_ui.bat          :: 起后端 + 托管界面 → http://127.0.0.1:8765/
15\scripts\run_selftest.bat    :: 静态契约自检（138 断言，需 Node）
```

**`15/` 是把 `14/` 的接口变成人能看的东西。** 14/ 开了 24 个 REST 接口，
但接口是给机器看的 —— 没有界面，运维人员要回答"昨天发生了什么"仍然得自己拼 `curl`。

```
   10/build ──导入──► 14/ SQLite ──REST(24)──► 15/ 界面（9 页）
                          ▲                        │
                          └── 静态托管（14/src/server.py --static 15）
```

| 事项 | 说明 |
| --- | --- |
| **零依赖、零打包** | 6 个文件、1692 行纯静态；图表是**手写 SVG**，不引 ECharts / Chart.js —— 断网可开、无版本漂移（同 `10/src/sim_report.h` 的路数） |
| **接口对齐被自检强制** | 自检解析 `14/src/api.py` 的 `ROUTES`，逐条核对前端调用的每个 `/api/*` 后端是否真有（含 `{id}` 参数化路径）。写错一个字母，**构建时就红**，不用等到点开那个页面 |
| **前端过滤不是安全边界** | 菜单按角色隐藏「用户与权限」，绕过 hash 也进不去；但真正的门在 `14/` 的 `ROUTES` 里（越权 403 / 未登录 401） |
| **不假装下发成功** | 控制指令下发在无真实执行器时显示"本次只记录意图并审计"，与 `14/` 的接口语义一致 |

**9 页**：总览 / 实时监控 / 历史曲线 / 告警中心 / 策略配置 / 收益分析 / 报表系统 /
设备管理 / 用户与权限。**登录页不回填、也不列出任何口令** —— 演示账号记录在
`15/docs/README.md` §权限（默认口令上现场前必须改，见 `14/docs/README.md` §5）。

**不能双击 `index.html` 打开**：界面要调 24 个 `/api/*`，`file://` 下浏览器按跨源直接拦掉，
必须由 `14/src/server.py` 托管 —— 跑 `15\scripts\run_ui.bat` 即可。

**没装 Node 怎么办**：`run_selftest.bat` 打印 `[SKIP]` 并以 0 退出。环境缺失不是代码缺陷，
但**必须显式喊出来**（同 `13/` 第三层跨语言层的处理），
否则"静默跳过"与"真的跑过"在输出上不可区分。

详见 [`15/docs/README.md`](./15/docs/README.md)。

### 2.21 通信加固：把"实验室里不触发、现场天天触发"的欠项补掉（`16/` 产出）

```bat
16\scripts\build_test.bat      :: 五层测试（3898 断言；TLS 层需 python 当对端）
16\scripts\build.bat           :: 只编译不跑（--no-test 时用）
```

`docs/交接/下一步工作交接说明.md` §3.5 列了 7 条工程欠项，`16/` 做掉其中 **6 条**：

| 欠项 | 原状 | `16/` |
| --- | --- | --- |
| 断线重连 | `13/` 固定间隔 | 指数退避 + 抖动；**`factor=1.0, jitter=0` 可逐位复现旧行为** |
| 主备双链路 | 无 | 连续 N 次失败切备、M 次成功切回（**M>N 形成滞环防抖**） |
| SNTP 对时 | 无 | RFC 4330 单播；**1900↔1970 纪元差 2208988800 s**；**回拨钳制**（SOE 时标不许倒退） |
| Modbus RTU | 只有 TCP | CRC16 + **3.5T 静默间隔分帧**（做成显式入参 `feed(p, n, gap_ms)`），垃圾字节重同步 |
| FTP 定值下发 | 无 | PASV / 多行应答 / `150`-`226` 分段 |
| TLS | 明文 | **Windows SChannel 真实现**，与 Python `ssl`（OpenSSL）端到端握手 + 逐位回显；非 Windows **显式拒绝，绝不退化成明文** |

**TLS 为什么必须拿别人的实现当对端**：自己写两端 = 自己批改自己的卷子。
SChannel 客户端配 SChannel 服务端，双方共享同一个 bug 时测试照样全绿。

**6 次反证**（"把实现故意改错，这条断言会红吗"）。其中两条最有代表性：
无滞环的对照实现在同一序列下切换 ≥ 10 次（否则"切换次数上界"可能只是条件没触发）；
关掉证书校验后**同一台**服务端必须连得上（否则"自签被拒"的原因就不一定是证书）。

详见 [`16/docs/README.md`](./16/docs/README.md)（含 SChannel 接入步骤与 OpenSSL 的取舍）。

### 2.22 设备全点表与高保真模拟器：把"假联调"里假的那部分换成真的（`17/` 产出）

```bat
17\scripts\build_test.bat      :: 六组测试（721 断言，零第三方依赖）
```

补的是 `docs/规划/模拟器与设备接入梳理.md` §10 的 **B 类（通路缺失）+ C 类（规模问题）**：

| 缺口 | 原状 | `17/` |
| --- | --- | --- |
| B2 全点表 | 只有 EMS 视角 **40 点** | 新建**设备侧全点表**（BMS 单体/绝缘/告警字、电表三相/电能/需量、PCS 无功/模式/故障码） |
| B3~B5 模拟器 | 电表 = **两个 `sin()`**、BMS = **6 点** | 三个高保真模拟器；**不许有恒为常量的量**（有断言证明每个关键量在变化） |
| C1 批量写 | `rt_db_set_multiple_values` **零调用** | N 点**一次调用** |
| C2 无变化上传 | 全量重发 | 死区；**安全点例外（死区 0 = 永远发布）** |
| C3 无分级节拍 | 单拍全量 | 快 0.1 s / 中 1.0 s / 慢 5.0 s |
| C4 两层点表 | 无映射 | 抽象表 ←→ 全点表，**映射表是代码不是文档** |
| C5 点表配置化 | **换站要改源码重编** | JSON 加载，往返幂等，可追加点不破坏既有索引 |

**量化结果（不是"做了优化"，是"从 X 降到 Y"）** —— 102 点 / 600 拍动态工况：

| 口径 | 全量重发 | 优化后 | 降低 |
| --- | ---: | ---: | ---: |
| **API 调用次数** | 61200 | **600** | **102.0×** |
| 点值写入次数 | 61200 | 13184 | 4.6× |
| ├ 安全点永久发布（**不可压缩的地板**） | 10200 | 10200 | 1.0× |
| └ 数据点 | 51000 | **2984** | **17.1×** |

**死区抑制率必须分场景说**：静止工况 **> 99%**、动态工况 **59.5%** ——
两个数字都真实，不许互相替代着用。

详见 [`17/docs/README.md`](./17/docs/README.md)。

### 2.23 性能基准与 SLA 门禁：把"性能达标"从一句话变成断言（`18/` 产出）

```bat
18\scripts\build_test.bat      :: 91 断言（含 23 条 SLA）+ 自动生成基准报告
```

★ **本步时序敏感**：它测 p99 / 加速比 / 内存增长，前提是**无并发负载**。
`build_all.bat` 是逐步串行的，满足这个前提 —— 但**不要并行跑第二次全量构建**。

| 基准 | 实测 | 判据 |
| --- | ---: | --- |
| 单拍闭环 `EmsRuntime::step(0.1)` p99 | **0.0042 ms** | SLA ≤ 10 ms（裕度 **2381×**） |
| 24h 长跑墙钟加速比 | **23943×** | ≥ 1000× |
| 发布步 p50/拍（基线 → 优化后） | 80.6 µs → **58.1 µs** | ratio **0.721** ≤ 0.95 |
| 长跑内存增长 | **16.4 MB**（句柄 delta = 0） | 有界 ≤ 64 MB |
| 末 1000 拍 / 头 1000 拍 p99 | ratio **0.667** | ≤ 2.0（**不随运行时长退化**） |

**23 条 SLA 全部通过**。每条都标注**来源**（设计推导 / 实测裕度 / 继承），
**不允许拍脑袋**；并配**反向验证**（人为加延迟，断言必须红）。

**最容易做错的一条**：`17/` 的 102× 是**API 调用次数**的降低，**不是 CPU 时间**的降低
（实测 CPU 只快 1.33~1.39×）。把前者当后者讲，是技术报告里最常见的夸大。

**数字可信边界**写在 `18/docs/README.md` §6：只在「本机 + 无并发负载 + 已预热」
三者同时成立时可引用；换机器必须重跑，不能搬数字。

详见 [`18/docs/README.md`](./18/docs/README.md) 与 `18/build/BENCH-REPORT.md`。

### 2.24 现场部署 P5：从"手动跑 exe"到"装到站上、有人守着、配置能回滚"（`19/` 产出）

```bat
19\scripts\install_task.bat        :: 注册开机自启计划任务（先看 --dry-run）
19\scripts\run_supervisor.bat      :: 前台跑守护器（调试用）
19\scripts\build_test.bat          :: 四套测试（424 断言）
```

**部署形态选了计划任务**（`schtasks /SC ONSTART /RU SYSTEM /RL HIGHEST`），不是 Windows 服务。
核心理由：真·Windows 服务要在 Python 侧做 SCM 握手，就得引 `pywin32`（违反"零第三方依赖"）；
而**不握手会被 SCM 在约 30 s 后杀掉，`sc query` 只显示 `STOPPED`** ——
现场看到的是"服务自己崩了"，真因是根本没握手。
所以 `install_service.bat` **故意拒绝安装**（退出码 **2**，不是 1），只打印配方与换路条件。

四块内容：

| 块 | 判据 |
| --- | --- |
| 守护器 `ems_supervisor.py` | 按 `11/` §2 的**硬依赖顺序**拉起（初始化器最先、就绪后才起下游）；**就绪超时 = 启动失败**（不许退化成 `sleep`） |
| 配置版本 `config_store.py` | `snapshot / list / diff / rollback`；**回滚前自动再存一版**（版本数 **+1** 而不是 −1） |
| 日志轮转 `log_rotator.py` | 大小 + 条数双阈值；**不丢事件**。轮转必须接住 P2 `SoeLog` 的有界 4096 条 —— 条件是 `导出周期 T × 最大事件速率 r < 4096` |
| **`docs/现场作业指导书.md`** | 从**空服务器**到 **EMS 在跑并接上真实位号**的可复现步骤（命令原文 + 期望输出 + 排查方向 + 点表逐点核对清单 + 验收判据 + 回滚应急） |

**"故障后不自动带载"是安全规程，不是缺陷** —— 这句话写进了指导书。

详见 [`19/docs/README.md`](./19/docs/README.md) 与 [`19/docs/现场作业指导书.md`](./19/docs/现场作业指导书.md)。

### 2.25 告警能力独立 + 持久化：清掉"出了问题看不见 / 恢复不了"（`20/` 产出）

```bat
20\scripts\build_test.bat      :: 三个测试（397 断言）
20\build\alarm_demo.exe        :: 告警 + 持久化端到端演示
20\build\soe_dump.exe --dump   :: 读 SOE 落盘文件（过滤 / 导出）
```

清的是交接文档 §3.6 点名「**优先清掉**」的三条：

| 债 | 现场后果 | `20/` |
| --- | --- | --- |
| 告警能力**寄生在仿真装配层**（入参是 `Sim24hConfig`） | 换个入口（`11/main_ems.cpp`、`14/` 后端）就**没有告警** | `alarm_model.h` + `alarm_input.h` + `alarm_assembler.h`，输入改成"与仿真无关"的快照；**同一套装配器同时服务仿真入口与生产入口**（有断言正面证明） |
| SOE 只在内存（`06/history()`）+ P2 `SoeLog` 有界 4096 | 长跑丢历史、重启丢迁移链 | `soe_store.h`：**文件是唯一真相源**，一条事件一行；落盘**先于**覆盖 |
| 配置只有 in-memory | 现场调参**一重启就没了** | `config_store.h`：往返一致 + 版本 + "谁在什么时候改了什么"；缺字段**指名报错**，不许默认值 |

**六类故障源的等级与处置与 `11/docs/README.md` §4 逐条对齐**（`20/docs/README.md` §4.4
有对齐表）；事件码 / 来源**复用 P2**，不另造一套。

详见 [`20/docs/README.md`](./20/docs/README.md)。

### 2.26 链路中间人：把"链路不正常时两端各是什么行为"变成一条命令（`13/` 产出）

```bat
13\.venv\Scripts\python.exe 13\sim\link_tap.py --listen 15021 --upstream 15020
13\.venv\Scripts\python.exe 13\sim\link_tap.py --ctl scenario blackout   :: 跑动中改工况
13\.venv\Scripts\python.exe 13\tests\test_link_tap.py                    :: 53 断言
13\.venv\Scripts\python.exe 13\tests\e2e_link_tap_live.py                :: 29 断言（需 pymodbus）
```

补的是原先**没有任何资产覆盖**的三件事：

| 缺口 | 现场后果 | `link_tap.py` |
| --- | --- | --- |
| **报文级不可见** | 值变成天文数字或 `0.0000` 时**两端日志都写"成功"** | 帧表显示完整 hex + 按从站口径解码的工程量 |
| **工况整轮固定** | `--bms-dis-forbid` 换一次要重启进程，"发生→持续→恢复"在**一次会话里跑不出来** | `timeline` 时间轴，跑动中切换 |
| **异常链路不可复现** | 间歇丢包 / 抖动 / 瞬断只在现场偶发，边界永远测不到 | 9 个预设工况，`--ctl` 一条命令 |

**两端零改动**：主站改连 tap 的端口，从站还是原来那份 —— 测的仍是真实交付物，
拔掉 tap 即恢复原状。

★ **实测抓到的关键发现**：把真值 `123 kW` 按**错误字序**发出后，C++ 主站读到
`0.0000` 且**质量标记是 ok** —— `kNonFinite` 守卫只挡 NaN/Inf，挡不住量程错值。
**两端之间没有任何告警。** 这正是模块文档 §2.1 那条纪律（"把读失败当读到 0"
是最典型的静默失败）在**另一个入口**的复现。

详见 [`13/docs/链路中间人_link_tap.md`](./13/docs/链路中间人_link_tap.md)。


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
         14/ 是**平台层**（框架第四层）：读 10/ 的产物与 04/ 的策略表，对上开 REST；
                                   **不参与实时闭环**，也不被任何模块依赖
         15/ 是**平台层前端**：只消费 14/ 的 REST，自身零依赖零打包；
                                   **不编译、不进任何 C/C++ 依赖图**
         16/ 是**通信加固层**：只提供传输/时间策略头文件，**不被任何模块依赖**；
                                   有意**不改 13/** —— 接入点写在 16/docs/README.md §6.4
         17/ 是**设备建模层**：01~16 都没变，它**并置**出一层设备侧全点表与三个高保真模拟器；
                                   **不改 07/ 的 40 点抽象表**（两张表靠映射表对账，D3 纪律）
         18/ 是**仪器层**：**只读地**引用 07/13/17/20 的头文件测性能，谁都不改（用 -I，不用 include 依赖）
         19/ 是**部署运维层**：只**调用**其他模块的产物（P1 的 ems_config.exe、11/ 的三进程、
                                   13/ 的 probe），不链接、不 include
         20/ 是**告警与持久化层**：从 06/ 的 StateEvent 等既有类型装配告警；
                                   `-I` 包含 06/ 的头文件复用，不改它们
         vendor/ 是**第三方库快照**：只增不改，上游与 P3/ 的改动都不往里面写
         09/ 10/ 11/ 12/ P3/ 14/ 16/ 17/ 18/ 19/ 20/ 都不产出被其他模块依赖的头文件
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
| `16/` | `src` | 通信加固层：**header-only、零依赖**。五层测试各自链 `-lws2_32`，TLS 层另需 `-lsecur32 -lcrypt32`（SChannel）与**外部 python**（当 TLS 对端）。**不 include 任何其他模块的头文件**，也不被任何模块 include |
| `17/` | `src` `tests` `../07/src/rtdb` `../07/vendor/rt_db` | 设备建模层：链 `ems_point_table.o` + `rt_db_api.o` + `ems_rt_db_setup.o`（复用 `07/` 的点表真相源与 RT_DB，**只读不改**）。**不需要** Python / pymodbus / Node / lib60870 |
| `18/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` `../09/src` `../10/src` `../11/src` `../13/src` `../17/src` `../20/src` `../P1/src` `../P2/src` + `../07/src/rtdb` `../07/vendor/rt_db` | 仪器层：**对上游全部只读**（只用 `-I` 引用头文件，不改任何一行）。自己编 `07/` 的三个 `.c` 到 `18/build/`（RT_DB 是 C 链接，不能只在 C++ 侧链头文件） |
| `19/` | —（Python，无 `-I`） | 部署运维层：只**调用**其他模块产物（`P1/build/ems_config.exe --capture`、`11/` 三进程、`13/sim`）。缺产物时相关用例显式 SKIP |
| `20/` | `src` `../04/src` `../05/src` `../06/src` `../07/src` `../08/src` `../07/src/rtdb` `../10/src` `../P2/src` | 告警与持久化层：`-I` 包含 `06/` 的 `StateMachine` 等既有头文件**复用**（不改它们）；等级/事件码复用 `P2/` 与 `14/schema.sql` 的既有定义，不另造 |
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
- **模拟器与设备接入梳理（四个接口方向 / 两层点表 / 指令下行两条路径 / 缺口清单）** → [`docs/规划/模拟器与设备接入梳理.md`](./docs/规划/模拟器与设备接入梳理.md)（**§10 = 缺口清单**。**A1 / A2 / B1 / A3.1 已交付**；**B2~B5 / C1~C5 已由 `17/` 补齐**；D 类仍未做）
- **产品化 P0：算法 ↔ 设备解耦（IDeviceIO / SimDeviceIO / MemoryDeviceIO）** → [`docs/产品化/P0-架构分层.md`](./docs/产品化/P0-架构分层.md)
- **产品化 P1：配置化（字段绑定表 / 装配顺序 / 校验 / 模板与文档生成）** → [`P1/docs/README.md`](./P1/docs/README.md)
- **产品化 P2：可观测性（边沿检测→SOE / 时间窗抑制 / O(1) 增量指标 / 指标与 log_every 解耦 / 三个正交旋钮）** → [`P2/docs/README.md`](./P2/docs/README.md)
- **产品化 P3：IEC104 从站网关（点表真相源 / 品质三态 / ASDU 分包 / 现场部署 / 6 个缺陷复盘）** → [`P3/docs/README.md`](./P3/docs/README.md)、[`P3/docs/design.md`](./P3/docs/design.md)
- **lib60870 第三方库接入（来源 / commit / 编译参数 / 两个坑 / GPL-3.0 授权说明）** → [`vendor/lib60870/README.md`](./vendor/lib60870/README.md)
- **RT_DB 接入：共享内存实时库适配器（跨内存边界闭环等价 / 点表契约 / 双连接 / Windows 存活句柄）** → [`07/docs/README.md`](./07/docs/README.md) §8
- **多策略协同的接口约定** → [`docs/接口规范/EMS策略接口规范.md`](./docs/接口规范/EMS策略接口规范.md)（§2.5 仲裁算法即 04/strategy_arbiter.h 的实现依据）
- **通信加固：重连退避 / 主备双链路 / SNTP 对时 / Modbus RTU / TLS / FTP 定值下发（工程欠项 6/7）** → [`16/docs/README.md`](./16/docs/README.md)（**TLS 走 Windows SChannel，与 Python ssl 端到端验过**；§3 八个坑、§6.3 接入步骤）
- **设备全点表与高保真模拟器（缺口清单 B 类 + C 类）** → [`17/docs/README.md`](./17/docs/README.md)（§4 **C1/C2/C3 量化结果**：API 调用 61200 → 600；§3 十一个坑）
- **性能基准与 SLA 门禁（23 条硬断言 + 24h 长跑）** → [`18/docs/README.md`](./18/docs/README.md)（§6 **数字可信边界** —— 性能数字没有可信边界等于没有）
- **现场部署 P5（守护器 / 配置版本回滚 / 日志轮转 / 作业指导书）** → [`19/docs/README.md`](./19/docs/README.md)、[`19/docs/现场作业指导书.md`](./19/docs/现场作业指导书.md)
- **告警能力独立 + SOE 持久化 + 配置持久化（清三条技术债）** → [`20/docs/README.md`](./20/docs/README.md)（§4.4 六类故障源与 `11/`§4 的逐条对齐表）
- **新模块开发约定（工具链 / 测试骨架 / `.bat` 三条坑 / 文档结构 / 纪律速查）** → [`docs/规划/新模块开发约定.md`](./docs/规划/新模块开发约定.md)

---

## 5. 代码组织原则

| 原则 | 说明 |
|---|---|
| **src/tests 分层** | 每个模块内部按 `src/` 源代码、`tests/` 测试、`samples/` 示例、`data/` 仿真 CSV、`figs/` 图、`docs/` 文档、`scripts/` 构建运行脚本、`build/` 产物分开。 |
| **共享代码唯一副本** | `03/shared/clamper.h` 是三个控制器共用 clamp，原本每处一份（共三份 + `ar_clamp` 模板），现在统一为 `bms::clamp`。 |
| **集成项目不复制源码** | `03/integration/src/IntegrationMain.cpp` 通过 `-I` 引用三个子项目的 header，源码不复制（原本有 6 份 `*.h/*.cpp` 重复，已删除）。 |
| **构建可重现** | 每个模块一个 `scripts/build.bat`；顶层 `scripts/build_all.bat` 串联全部。修改任何模块后只需重跑所在模块的 build.bat。 |
