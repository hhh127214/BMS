# 07/ —— 周期 7：实时控制闭环

> 本模块是 [`工商业储能EMS调控策略设计方案.md`](../工商业储能EMS调控策略设计方案.md) §7 **周期 7** 的落地实现，
> 打通「电表实时数据 → EMS算法计算 → 策略仲裁 → 安全约束 → PCS执行 → 实际功率反馈 → EMS迭代修正」全链路。

| | |
| --- | --- |
| 对应周期 | 周期 7：实时控制闭环 |
| 周期目标 | 「实现毫秒级实时控制闭环，无延迟、无振荡。」 |
| 形态 | **纯头文件 C++17 库**（`src/plant_model.h` + `src/realtime_loop.h`）；`src/main.cpp` 为单场景演示 |
| 依赖 | `04/src`（策略层 + 仲裁器）、`05/src`（安全兜底）、`06/src`（状态门控）、`08/src`（协同层） |
| 单元测试 | `tests/test_realtime_loop.cpp`，**T11~T16 / 6050 断言，全过** |
| 演示 | `src/main.cpp` → 场景 C：阶跃跟随 + 抖动治理三档对照 + 变化率对照 |
| 关键常量 | 控制周期 `dt = 100 ms`；实时层纠偏上限 `l2_correction_max_kw = ±100 kW`；输出死区 `2 kW` / 滞环 `3` 拍 |
| 设备 I/O 抽象 | 接口在 `04/src/device_io.h`（P0）；本模块提供 3 个实现：`src/sim_device_io.h`（仿真）、`src/memory_device_io.h`（P0.5 进程内点表）、`src/rtdb/rtdb_device_io.h`（RT_DB 共享内存） |
| RT_DB 接入测试 | `tests/test_rtdb_device_io.cpp`，**T25~T29 / 546 断言，全过**（`scripts\build_test_rtdb.bat`） |

---

## 1. 快速开始

```bat
cd 07
scripts\build.bat          :: 编 test_realtime_loop.exe + loop_demo.exe
scripts\build_test.bat     :: 编 + 跑 T11~T16（应输出 PASS=6050 FAIL=0 / ALL TESTS PASSED）
scripts\build_test_rtdb.bat:: 编 + 跑 RT_DB 接入 T25~T29（应输出 PASS=546 FAIL=0）
scripts\run_demo.bat       :: 跑场景 C，输出 → build\demo_output.txt
```

> **编译依赖**：`src` + `../04/src` + `../05/src` + `../06/src` + `../08/src`。
> `EmsRuntime` 装配了 `08/` 的协同层，故需 `-I ../08/src`。这是**运行时调用关系**
> （闭环调用优化层），头文件层面**无环** —— `08/src/*.h` 不包含本模块任何头文件。

---

## 2. 文件导览

```
07/
├── src/plant_model.h              ← 被控对象：PCS 死区 + 一阶惯性 + 变化率 + 效率 + 温升 + 噪声/故障注入
├── src/sim_device_io.h            ← P0 仿真适配器：把 PlantModel 接到 IDeviceIO
├── src/memory_device_io.h         ← P0.5 进程内点表适配器（RT_DB 的进程内等价物）
├── src/rtdb/rtdb_device_io.h      ← **RT_DB 接入**：共享内存实时库适配器 + 设备侧写点器
├── src/rtdb/ems_point_table.h|.c  ← 40 点 EMS 点表（= 32 设备侧 + 8 EXT；C/C++ 共用，点名与 mem_point:: 逐字相同）
├── src/rtdb/ext_setpoints.h       ← ★ 读侧：EXT 外部设定的读取 + 失效判定（A3.1，纯 header）
├── src/rtdb/ems_rt_db_setup.h|.c  ← 点表初始化器（建段 + 注册 40 点；补 RT_DB 无注册 API 的缺口）
├── src/realtime_loop.h            ← EmsRuntime 11 步闭环 + ⓪ 限值刷新 + OutputShaper + LoopConfig/StepRecord/LoopMetrics
├── src/main.cpp                   ← 场景 C：3 个试验
├── tests/test_realtime_loop.cpp   ← T11~T16
├── tests/test_device_io.cpp       ← T21~T24（P0/P0.5 适配器可换性）
├── tests/test_rtdb_device_io.cpp  ← T25~T29（RT_DB 接入：点表契约 / 跨内存边界闭环等价 / 双连接 / 故障）
├── vendor/rt_db/                  ← RT_DB 源码快照（rt_db_api.c/.h + structs + private）
├── docs/README.md                 ← 本文件
├── docs/design.md                 ← 设计说明（闭环时序 / 第⑨步顺序 / 整形语义 / 指标口径 / 抖动治理）
├── scripts/{build,build_test,build_test_device_io,build_test_rtdb,run_demo}.bat
└── build/                         ← 产物（git ignore；`build\demo_output.txt` 为演示输出）
```

---

## 3. 闭环链路（`EmsRuntime::step()` 的 11 步 + ⓪ 前置刷新）

```
电表实时数据 → EMS 算法计算 → 策略仲裁 → 安全约束 → PCS 执行 → 实际功率反馈 → EMS 迭代修正
```

| 步 | 动作 |
| --- | --- |
| **⓪** | **设备限值刷新（2026-09-19 补，A1）**：`if (cfg_.refresh_limits_each_step) refresh_device_limits();` —— 必须在①之前，**本拍的仲裁/安全用的是本拍的限值**。是否开启跟随适配器能力（`io_->limits_are_live()`），见 §8.6 |
| ① | 采集：`plant_.sample(t_)` 产出**冻结**的 `RealtimeSnapshot`（接口规范 §5 强约束） |
| ② | 故障检测：`detect_faults()` |
| ③ | 安全评估：`safety_.evaluate()` → `SafetyVerdict` |
| ④ | 状态机：`fsm_.update()`（`06/`） |
| ⑤ | 协同层：`coord_.update()` → `plan_target` / `coord_target`（`08/`） |
| ⑥ | 策略层：`mgr_.tick()` + 安全结论并入（`04/`） |
| ⑦ | 仲裁层：`arbiter_.arbitrate()`（L0→L3 区间收敛 + L3 desired 加权） |
| ⑧ | 实时层纠偏叠加（L2 有界修正） |
| ⑨ | **输出整形 → 安全兜底 → 状态门控 → HOLD_LAST** |
| ⑩ | 执行层：`plant_.step()`（PCS 物理响应） |
| ⑪ | 反馈 + 记录：实测功率回灌下一拍快照，形成闭环 |

### 第 ⑨ 步的顺序（关键设计）

```
OutputShaper（死区/滞环去抖） → SafetyEngine::apply()（区间限幅 + 变化率限速）
  → 状态机门控（FAULT/EMERGENCY 强制 0，覆盖一切） → HOLD_LAST
```

- **整形放在最后一级**：`04/` 仲裁器的死区只作用于 L3 的 `desired`，而 L2 实时纠偏是在仲裁
  **之后**叠加的，抖动会绕过仲裁器死区。必须在链路末端再放一道整形。
- **安全层在整形之后**：安全兜底必须无条件生效，不能被整形"软化"。
- **状态机在安全层之后**：FAULT / EMERGENCY 必须能覆盖一切（包括安全层的限幅结果）。

---

## 4. 场景 C 演示（`scripts\run_demo.bat`）

### 试验 1：阶跃跟随与倒送抑制（无延迟）

```
  判定：无延迟=成立  无振荡=成立
  说明：阶跃瞬间的暂态倒送源于 PCS 物理惯性（0.2s 死区 + τ=0.5s），
        稳态段关口功率已稳定在 0 附近，防逆流约束生效。
```

### 试验 2：并网点边界抖动治理三档对照（无振荡）

激励：净负荷恒为 0，量测噪声 `σ = 3 kW`（关口功率 `σ ≈ 4.2 kW`）。

| 配置 | 指令总行程 | 方向反转 | 大幅跳变 | 无振荡判定 |
| --- | --- | --- | --- | --- |
| ① 基线（无滤波/无死区） | 4548 kW | 809 次 | 266 次 | 不成立 |
| ② + 量测滤波（α=0.05） | 3671 kW（−19%） | 689 次（−14%） | 199 次（−25%） | 不成立 |
| ③ + 量测滤波 + 死区滞环 | 1326 kW（**−70%**） | 183 次（**−77%**） | 185 次（−30%） | **成立** |

结论：**根因在量测侧**（安全边界的基准量 `base = P_load − P_pv` 带噪声），
量测低通滤波是主药，输出死区+滞环是辅药。

### 试验 3：变化率对照（工程折中）

| 变化率 | 最大倒送 | 倒送时长 | 安全裁剪 |
| --- | --- | --- | --- |
| 50 kW/s（安全层限） | 148.0 kW | 2.8 s | 1 次 |
| 2000 kW/s（物理层限） | 148.0 kW | 2.9 s | 0 次 |

结论：峰值倒送由**光伏阶跃瞬间的实际出力**决定，与变化率无关；
根治手段是光伏逆变器限功率或提前预留充电裕量。

---

## 5. 单元测试覆盖（T11~T16）

| 用例 | 覆盖点 |
| --- | --- |
| T11 | 输出门控不变量（闭环口径：非运行态恒 0） |
| T12 | 电表通信异常走 HOLD_LAST，不进 FAULT |
| T13 | 被控对象动态（死区/惯性/变化率/效率/SOC） |
| T14 | `OutputShaper` 死区清零 / 滞环 / 关闭时透传 |
| T15 | `LoopMetrics` 行程/反转/翻转/倒送统计 |
| T16 | 闭环端到端 —— 指令跟随、无倒送、安全兜底 |

---

## 6. 验收指标口径（`LoopMetrics`）

| 指标 | 口径 | 判定阈值 |
| --- | --- | --- |
| `mean_err` / `rmse` | `P_actual − P_cmd` 的均值 / 均方根 | — |
| `steady_err` | 末段 10% 的平均绝对误差 | `≤ 15 kW`（`no_lag`） |
| `over_delivery` | `Σmax(0, \|P_actual\| − \|P_cmd\|) / Σ\|P_cmd\|` | `≤ 25%`（`no_lag`） |
| `sign_flips` | 指令穿越零点次数（0.5 kW 死区） | `≤ 0.20/s`（`no_oscillation`） |
| `cmd_reversals` | 指令增量符号反转次数（`dir_eps = 2 kW`） | `≤ 1.00/s`（`no_oscillation`） |
| `cmd_travel` | `Σ\|Δcmd\|`，输出"抖动总量"的标准度量 | — |
| `max_reverse` / `reverse_duration` | 有效倒送（`P_grid < −5 kW`）的峰值 / 累计时长 | — |

> **为什么用 `cmd_travel`**：`cmd_reversals` 会把"死区造成的 bang-bang 输出"也计入反转，
> 无法反映死区的收益；`Σ|Δcmd|` 才是衡量 PCS 无效往复动作的正确口径。

---

## 7. 设计文档

闭环时序、第 ⑨ 步顺序论证、`OutputShaper` 语义、被控对象建模顺序、指标口径、
抖动三层治理的完整设计说明见 [`docs/design.md`](./design.md)。

---

## 8. RT_DB 接入（共享内存实时库适配器）

P0.5 只用 `MemoryDeviceIO` 证明了「换数据源不改算法」在**进程内**成立；
现场换掉的是**另一个进程**。本节把 RT_DB（共享内存实时库）接进 `IDeviceIO`，
把那条证明从「同一容器」升级到「同一段物理内存」。

### 8.1 两个方向，两个人

```
        写 MEAS/STA/CFG ↓                         ↑ 读 MEAS/STA/CFG
   设备 / SCADA 进程（RtDbPointWriter）       EMS 进程（RtDbDeviceIO）
        ↑ 读 CMD.*                              ↓ 写 CMD.*（指令 + 权限区间）
                 └──────── RT_DB 共享内存段 ────────┘
```

- **设备侧**只写 `MEAS.*` / `STA.*` / `CFG.*`；**EMS 侧**只写 `CMD.*`。
  这条边界就是现场「谁拥有这个点」的约定，越界会让事后追溯一团乱麻
  （`RtDbPointWriter` 与 `RtDbDeviceIO` 分成两个类，就是为了让越界变成编译期错误）。
- 算法层**看不到任何点名**：它只经 `IDeviceIO` 拿 `RealtimeSnapshot` /
  `DeviceLimits` / `DeviceStatus`，指令也只经 `PowerCommand` 出去。

### 8.2 `execute()` 的语义（与仿真适配器的根本区别）

| 适配器 | `execute()` 做什么 | 返回值 |
| --- | --- | --- |
| `SimDeviceIO` / `MemoryDeviceIO` | 积分被控对象（死区/惯性/变化率/SOC） | 本拍实际功率（可信） |
| `RtDbDeviceIO` | **只把指令写进实时库 + 等一拍**（真实 PCS 亦如此） | 本拍量测的「尽力反馈」，**仅供记录** |

因此真实适配器下**闭环必须走下一拍的 `read_snapshot()`**（接口契约 ③）。
单进程测试/演示需要闭环时，用 `set_device_pump()` 把「设备进程」搬进本进程
（不设置 = 纯下发，不做任何物理积分）。

### 8.3 采集失败与品质位

- 读点失败时**保留最近一次有效值**并计入 `stale_reads()`（契约 ①：绝不返回半成品）。
- `data_valid` = 实时库的数据有效位 **且** 量测点品质位全为 GOOD。
  品质位**随采集刷新**，所以 `read_status().data_valid` 反映「最近一次采集」——
  `EmsRuntime::step()` 固定先 `read_snapshot` 再 `read_status`，顺序天然正确。

### 8.4 运行

```bat
cd 07
scripts\build_test_rtdb.bat    :: gcc 编 C 实时库 → g++ 编适配器与测试 → 跑 T25~T29
                               :: 期望 PASS=546 FAIL=0 / ALL TESTS PASSED
```

| 用例 | 覆盖点 | 结论 |
| --- | --- | --- |
| T25 | 点表契约：共享内存点表 ↔ 编译期点表 ↔ 设备侧点表；常量/品质位漂移守卫；两个方向的读写；`CFG.*`→`DeviceLimits`；**BMS 禁充放位**（置位 / 互不串扰 / 归零可恢复 / 双置）；品质位语义 | 三方一致（40 点点名/单位/索引） |
| T26 | **跨内存边界闭环等价**：一路 `RtDbDeviceIO`（数据全经共享内存）+ 设备侧泵，一路直连 `MemoryDeviceIO`，装配序列/环境脚本完全相同 | 400 拍**逐位一致**（峰值指令 100 kW，SOC 0.5→0.4993） |
| T27 | 两个独立连接（两次 `rt_db_init`）看同一段内存：写 A 读 B、写 B 读 A、段级写计数共享 | 「跨进程」不是进程内副本 |
| T28 | 故障经共享内存驱动状态机：设备侧写 `STA.PCS_FAULT` → EMS 进 FAULT → 指令归零/撤销许可 → 恢复后停在 READY 不自动带载 | 设备→EMS 方向的真闭环 |
| T29 | **关口功率单一数据源**（2026-09-19 补，缺口 A2）：给电表注入**系统偏差** `meter_bias_kw=40`，使"三路相减的平衡值"（200 kW）与"电表读数"（240 kW）**可区分**；判定 `read_snapshot().p_grid_kw` 跟**电表**、`read_actuals()` 同口径；再以 `bias=0` 反向验证回到平衡值 | 两条路径同源；旧写法（推算）在这条判据下**必红** |

### 8.5 现场部署的三个约束（都是本次接入踩出来的）

1. **Windows 下初始化器不能是「短命进程」**：段是页面文件支撑的文件映射对象，
   最后一个句柄关闭即销毁。`ems_rt_db_setup()` 会保留一个本进程的存活句柄；
   若把初始化器做成 `init.exe` 跑完就退出，运行时必然报
   `Shared memory not found. Please run init tool first.`
   （Linux 的 `shmget` 段在 `shmdt` 后依然存在，所以同一份代码在 Linux 上「看起来是对的」）。
2. **初始化器必须在所有连接建立之前调用**：`reset` 会 `memset` 整个段，
   连接计数等段级元数据会被清零。
3. **一个进程一条连接**：RT_DB 的 `create/open_shared_memory()` 共用一个进程级全局
   句柄，因此装配层应统一持有一条连接（`RtDbDeviceIO(rt_db_handle_t*)` 借用），
   而不是每个适配器各自 `open_owned()`。

### 8.6 设备限值的**运行期刷新**（2026-09-19 补，A1）

`04/IDeviceIO::read_limits()` 的契约写的是「**每个控制周期刷新** —— 这是 BMS 动态
降功率能生效的唯一入口」。但 2026-09-19 之前，`EmsRuntime::step()` **从未调用**
`refresh_device_limits()` —— 它只在 `init` / `reset` / `configure_plant` /
`attach_device` 里被调用，也就是限值在运行期是**冻结**的。

后果远不止"注释不准"：

| 被冻结的字段 | 现场表现 |
| --- | --- |
| `bms_chg_forbidden` / `bms_dis_forbidden` | **BMS 禁充放完全不生效**（等于"永远允许充放"） |
| `bms_chg_limit_kw` / `bms_dis_limit_kw` | BMS 动态降功率不生效 |
| `pcs_rated_chg_kw` / `pcs_rated_dis_kw` | PCS 限值不跟随 |
| `transformer_capacity_kw` / `d_target_kw` | 变压器容量、契约需量不跟随 |

为什么三层测试全绿也没发现：仿真场景的限值由调用方**直接注入**
`rt.device_limits()`（`09/` `10/` `P1/` `P2/` 共 8 处），既绕开了点表、
也绕开了"每拍刷新"这条路。**只有"设备侧写点 → 共享内存 → 每拍刷新"这条
真实路径才照得出来。**

修法（`LoopConfig::refresh_limits_each_step`）：

```
IDeviceIO::limits_are_live()                // 接口新增，默认 false
  ├─ RtDbDeviceIO  → true                   // 限值在另一个进程里会变
  └─ SimDeviceIO / MemoryDeviceIO → false   // 限值来自配置，不存在"脚下会变"
EmsRuntime::attach_device()  → 若 limits_are_live() 则自动打开刷新
EmsRuntime::step()           → ⓪ 步：if (refresh_limits_each_step) refresh_device_limits()
```

为什么是**开关**而不是无条件刷新：`dev_` 有两个写入者 —— ① `read_limits()`
（设备侧说了算）与 ② 装配层注入（仿真/测试的夹具手法）。无条件刷新会让 ② 被
① 覆盖，现有 8 处仿真注入即刻失效。默认跟随适配器能力，所以**现场装配不会忘**、
仿真行为逐位不变。`07/T26` 这类"介质等价性"测试会显式关掉它（限值必须是固定夹具值）。

### 8.7 BMS 禁充放位的现场接入硬要求

`STA.BMS_CHG_FORBID` / `STA.BMS_DIS_FORBID` 是点表里**唯一两个安全输入型的状态点**
（索引 30/31，追加在末尾以免打乱既有索引）。它们经
`read_limits()` → `DeviceLimits` → 04/S01(`kBmsForbid`, **L0 最底层**) →
05/`check_bms_forbid()` → 折进 `(p_lower, p_upper)`。

两条现场要求：

1. **BMS 网关必须每拍显式写这两个点**。RT_DB 里"没被写过"与"写过 0"在**值上
   不可区分**（都是 0.0），所以不存在"不支持就不写"这个选项 —— 不支持时应写 0，
   并在自己的通信位 `STA.BMS_COMM_OK` 上体现。点表默认值是 **0（允许）**，刻意
   不设成 1：默认禁充放会让"设备侧尚未上线"直接锁死 `[0,0]`（冷启动不可用）。
2. **"掉线"这条 fail-safe 由 EMS 侧的通信位兜住，不由禁充放位承担**。
   05/ 的 `check_bms_forbid()` 判据是本快照的 `meters_alive["BMS"]`
   （来自 `STA.BMS_COMM_OK`），**独立**收紧到 `[0,0]`，**不依赖禁充放位的值**。
   所以默认值取 0（允许）不会造成"掉线后被误认为允许"；
   禁充放位负责的是另一件事：**在线、但要求禁充放**。
   网关要做的只是"在自己失效前把通信位置 0"，而不是让 EMS 去猜一个坏点该读成什么。

对照测试：`07/T25`（适配器层，置位/互不串扰/可恢复 四个方向）、
`11/T48`（跨进程联调，7 层判据含差分）、`12/A5-07`（验收层）。


### 8.8 关口功率的**单一数据源**（2026-09-19 补，缺口 A2）

**问题**：同一个 `p_grid_kw`，两个出口取了两个源 ——

```cpp
// 算法路径（修复前）：三路量测相减 —— "算出来的"
out.p_grid_kw = p_load - p_pv - p_bat;
// 记录路径
a.p_grid_kw = cached_value(EMS_P_GRID);   // 电表点 —— "表报的"
```

**为什么必须读电表**：真实系统里关口电表是**唯一权威计量点**。负荷/光伏/电池
三路各有 CT/PT 精度、滤波与通信周期差异，**相减是把误差叠加而不是抵消**，
而且偏差还随工况漂 —— 现场表现就是"报表里 100 kW、防逆流却按 137 kW 动作"。
两个消费者都是安全约束：**S05 防逆流**（`surplus = p_grid_min - rt.p_grid_kw`）与
**变压器过载**（`|p_grid| + 0.1·p_load`）。

**修法**（三个适配器口径统一）：

| 层 | 落点 |
| --- | --- |
| 唯一定义 | `PlantModel::meter_p_grid()` —— `sample()`（带噪声量测）与 `p_grid_actual()`（真值）都从它出，差别只剩"是否含本拍噪声" |
| 共享内存 | `RtDbDeviceIO::read_snapshot()` 改读 `EMS_P_GRID` |
| 进程内 | `MemoryDeviceIO::read_snapshot()` 改读 `mem_point::kPGrid`；新增 `set_meter_bias_kw()` |

★ **`set_environment()` 必须同步刷新电表点**：`MEAS.P_GRID` 原先只在 `execute()`
（拍末）更新，而负荷/光伏在 `set_environment()`（**拍首**）写入 → 电表读数与其它
量测**不在同一时刻**。"一次环境发布 = 同一时刻的一致量测集"，这是隐式契约。

★★ **这一段最重要的教训是"差点修了个寂寞"**：设备侧发布的 `MEAS.P_GRID`
**恰好等于**那个相减式（平衡式本来就成立），所以把 `read_snapshot()` 改成读电表点后，
**在所有既有夹具下逐位恒等** —— 编译、单测、验收全绿，但**把代码退回去一条断言也不会红**。
必须先加一个**默认 0、可注入**的偏差量（`meter_bias_kw`）把两条路径拉开，
契约才"有区分度"。**判据有效性已验证**：退回推算式 → `11/T49` 立刻 **6 条红**。

对照测试：`07/T25`（点表契约：电表 137 vs 平衡 157）、`07/T29`（适配器，偏差 40）、
`11/T49`（跨进程，200 拍）、`12/A1-09`（验收层）。

**遗留**：电表目前只建模为**常量系统偏差**，真机还差**一阶滞后**与**慢漂移**。

---

### 8.9 EXT 外部设定（调度 → EMS，A3.1）

**读侧在本模块**（`src/rtdb/ext_setpoints.h`），写侧在 `P3/`（IEC104 网关）。
分开的理由：网关只负责"把设定落进段里"，"这组值能不能用"是**消费侧**的判断。

| 函数 | 职责 |
| --- | --- |
| `load_ext_setpoints(read, now_s, stale_s, retries)` | 一次**一致**读取 6 个设定 + 时标，并给出五个独立的有效性标志 |
| `narrow_interval_by_ext(ext, &p_lower, &p_upper)` | 把设定折成对权限区间的**收窄**（A3.2 起由 `05/` SafetyEngine 作第 10 条约束调用） |

**为什么不逐点读**：`rt_db_get_value` 只保证**单点**不撕裂，而"上下界 + 设定 + 时标"
是一组应当同时生效的量。读到"新的上界 + 旧的下界"时，**中间态可能比两端都宽** ——
对安全区间来说那是最不能容忍的一瞬间。所以按 `EXT.SEQ` 做**区级 seqlock**：
`s0 == s1` **且 `s0` 为偶数** 才算一致（协议与"为什么偶数也算判据"见
`P3/docs/design.md` §5.3）。

**★ 五个有效性标志是正交的，不要合成一个 bool**：

| 标志 | 含义 | 处置 |
| --- | --- | --- |
| `present` | 段里收到过外部设定（`EXT.SEQ > 0`） | false → 用哨兵（= 无约束） |
| `stale` | `now - EXT.TS > stale_s`，**或**时标来自未来，**或** `stale_s <= 0` | → 整区无效 |
| `consistent` | 拿到了一致快照（区序号前后相同且为偶数） | → 本拍不施加 |
| `finite` | 数值全有限（NaN/Inf 会污染区间求交） | → 本拍不施加 |
| `band_ok` | `p_lower_set <= p_upper_set`（倒挂 = 调度配错） | → 本拍不施加 |

**★ 停机 / 闭锁在 stale 时粘住（A3.2 拍板）**：`usable()` 是给功率设定（6001~6004）用的。
停机 / 闭锁（5001 / 5002）是**状态意图**，`narrow_interval_by_ext()` 在 stale 时仍粘住 ——
守卫 = `present && consistent && finite`（**不含** stale / band_ok；含 finite 是为挡住
NaN 经 `>0.5` 误判成"停机"）。取舍见 `P3/docs/design.md` §5.7。

**★ A3.2 接线**：`narrow_interval_by_ext()` 现在是 `05/SafetyEngine::evaluate()` 的第 10 条约束，
在本地 9 条收敛之后、区间矛盾检查之前调用（只收紧，收紧后可能制造矛盾 → 统一走
DERATED 而非 EMERGENCY）。装配层（`EmsRuntime::set_ext_source()`）每拍读 EXT 快照并传入；
`11/src/main_ems.cpp` 用 RT_DB 句柄注入读回调，`11/` 设备侧 `publish_all()` 跳过 EXT 区。

**★★ 失效兜底不能只靠写者**：网关进程被 kill 时 `clear_all()` 不会跑 →
最后一个设定会**永远生效**。所以必须有一条**不依赖任何进程活着**的判据：
`now - EXT.TS > stale_s`。网关侧的清空只是"快路径"。

**两个容易写反的边界**：
- 时标**来自未来**（`now < ts_s`）必须判不可用 —— 否则一个未来时标会让"陈旧判定"
  **永远不成立**，等于把兜底关掉。
- `stale_s <= 0` 的含义是"**禁用外部设定**"（视为总是陈旧），不是"禁用超时判定" ——
  前者的失效方向是安全的。

**测试**：`P3/tests/test_iec104.cpp` T71（纯逻辑，假读回调穷举边界）+
`P3/tests/test_ext_rtdb.cpp` T76b/T76c（真段 + 真并发）。
