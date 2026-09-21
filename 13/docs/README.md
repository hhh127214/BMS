# 13/ — Modbus TCP 设备接入（PCS / BMS / 电表）

> 一句话：把 `IDeviceIO` 从"进程内仿真"推进到"**真的过网线**"。
> EMS 作 Modbus TCP **主站（客户端）**，向 PCS / BMS / 电表要数据、下发功率指令；
> 设备侧由 `13/sim/modbus_slave.py`（Python `pymodbus`）扮演，或换现场真实设备。

文档地图：
- 本文 = **是什么 / 怎么跑 / 设计在哪 / 已知限制**
- `docs/design.md` = 协议层与映射表的设计推演（为什么必须做字序、分块、品质合成）
- 各源文件头部 = 该类**具体**的踩坑记录（比本文详细，改代码前先读）

---

## 1. 它在整个工程里的位置

```
                    ┌──────────────────────────────────────────┐
                    │  07/ 实时控制闭环  realtime_loop.h        │
                    │  （只认 IDeviceIO 抽象，不认任何协议）     │
                    └──────────────────┬───────────────────────┘
                                       │ IDeviceIO
        ┌──────────────┬───────────────┼───────────────┬──────────────────┐
        ▼              ▼               ▼               ▼                  ▼
  SimDeviceIO   MemoryDeviceIO   RtDbDeviceIO    ModbusDeviceIO       (将来…)
  进程内物理模型   进程内点表    跨进程共享内存     13/ 本模块          IEC61850?
  (07/)          (P0.5)          (07/src/rtdb/)   Modbus TCP 主站
                                                        │
                                          ┌─────────────┴──────────────┐
                                          ▼                            ▼
                                  13/sim/modbus_slave.py          现场真实设备
                                  （Python 从站，跨语言联调）      （PCS/BMS/电表）
```

**四个适配器是同一接口的四种落地方式**。纪律：**对同一个物理量必须给出同一个语义**，
否则"换数据源不改算法"在数值上就不成立。本模块严格对齐了：
站用电计入负荷侧（`p_load = max(0, MEAS.P_LOAD + CFG.PCS_STANDBY)`）、
关口功率取**电表读数**而非三路相减、BMS 禁充放位语义与 A1 修复后的 RT_DB 路线逐点一致。

---

## 2. 三件事必须先理解，否则看代码会误判

### 2.1 ★ Modbus 没有品质位 —— "值可信吗"必须由主站**自己合成**

RT_DB 的每个点自带 `data_quality_t`（BAD/GOOD/UNCERTAIN），可信性是**读回来的**。
Modbus 里寄存器就是 16 位裸数据：没有单位、没有时标、没有品质。

后果：**把"读失败"当"读到 0"是最典型的静默失败** —— 0 kW 的负荷、0% 的 SOC
都是看起来完全正常的数值，算法会照常跑，只是跑在一个假世界上。

本模块的纪律：**失败时保留上一次有效值，可信性另行表达**（与 `04/src/device_io.h` 契约①一致）。

### 2.2 ★ 一次快照 = **一组**请求 → 部分成功是常态

一次 Modbus 事务只能读**一段连续地址**。32 个点被规划成 **3 次请求**
（见 §3）。于是"输入寄存器读到了、离散输入超时了"是运行常态。

用**两个独立的位**表达，不能混成一个计数：

| 计数 | 含义 | 现场排查方向 |
|---|---|---|
| `block_fails()` | 分块请求失败 | **通信 / 地址**问题（线、网关、从站地址、超时） |
| `decode_fails()` | 解码失败（字序/NaN/越界） | **映射表配错**（尤其 32 位字序） |

`point_valid(i)` = 所在分块本拍成功 **且** 该点解码成功。两个条件缺一不可。

### 2.3 ★ 32 位浮点字序 —— 现场第一大坑

Modbus 只规定"每寄存器 16 位、大端传输"，**没规定** 32 位量占的两个寄存器谁在前。

- `WordOrder::kHighWordFirst`（"ABCD"）
- `WordOrder::kLowWordFirst`（"CDAB"）

写错的表现**不是"差一点点"，而是数值完全离谱或 NaN**。

★ 映射表里 **`MEAS.P_BAT` 刻意用低字在前**，其余 f32 都用高字在前 ——
这是**故意的区分度**：若全部统一成一种，"把字序写死"的实现也能全绿，
而现场换一台设备就会错。T06 有反向守卫钉住这一点。

---

## 3. 点表映射（`src/modbus_point_map.h` —— **映射的唯一真相源**）

32 个点按表分三段，**段内地址连续是刻意的**（决定请求次数）：

| 表 | 段 | 地址 | 点数 | 编码 |
|---|---|---|---|---|
| 输入寄存器 IR（FC04，只读） | 量测 | 0..10 | 7 | f32×4 / u16×2 / i16×1 |
| | 空位 | 11 | — | 预留（不挪后面） |
| | 配置 | 12..39 | 14 | f32 ×2 寄存器 |
| 保持寄存器 HR（FC03/06/16，可写） | 指令 | 0..5 | 3 | f32 ×2 寄存器 |
| 离散输入 DI（FC02，只读） | 状态 | 0..7 | 8 | bit |

**读全 32 点 = 3 次请求**。这是**契约**不是实现细节：
T09/T22/`--plan` 都直接断言这个数字，改回"一点一次"立刻变 32
（现场"采一帧要 3 秒"多半就是点表散了）。

四条编译期守卫（`static_assert`）：

| 守卫 | 防的是 |
|---|---|
| `bindings_well_formed()` | 逐点覆盖 / 索引错位 / 编码与表类型不匹配 |
| `segments_consistent()` | 量测·配置·状态被误标可写，或指令点落到只读表 |
| `cmd_points_contiguous()` | **三个指令点地址不连续** → 无法一次 FC16 原子下发 |
| `blocks_cover_all_points()` / `blocks_disjoint()` | 有点没被任何分块覆盖（会恒为默认值）/ 分块重叠 |

### 指令必须**原子下发**

`write_command()` 一次 FC16 写 `(p_bat, p_upper, p_lower)` 共 6 个寄存器。
拆成三次的话，设备可能执行到"**新的功率 + 旧的权限区间**"的中间态，
而那一瞬间新指令可能已经越过旧区间。地址连续由 `cmd_points_contiguous()` 静态保证。

---

## 4. 怎么跑

### 4.1 构建

```bat
13\scripts\build.bat          :: → build\modbus_probe.exe（现场点表核对工具）
13\scripts\build_test.bat     :: → 三层测试，编译 + 运行
```

也可以从根目录跑（已挂在 `scripts/build_all.bat` 第 **30 / 31** 步）：

```bat
scripts\build_all.bat              :: 全量 31 步
scripts\build_all.bat --no-test    :: 跳过所有测试
```

### 4.2 起设备替身（Python 从站）

```bat
python -m venv .venv
.venv\Scripts\pip install -r 13\sim\requirements.txt
13\scripts\run_sim.bat --load 380 --pv 150 --bias 40
```

常用参数（都是**现场工况**的旋钮，用来复现问题）：

| 参数 | 用途 |
|---|---|
| `--load / --pv` | 负荷 / 光伏出力 |
| `--bias` | **电表系统偏差**。非 0 用于验证主站读的是电表口径（见 §5） |
| `--soc0 / --soh / --ambient-c` | 初始 SOC / SOH / 环境温度 |
| `--bms-chg-forbid` / `--bms-dis-forbid` | 模拟 BMS 上报禁充 / 禁放 |
| `--pcs-fault` / `--offline` | 模拟 PCS 故障 / 设备离线 |
| `--tau-s / --ramp-kw-s` | 一阶惯性时间常数 / 变化率上限（与 `07/` 同式） |
| `--self-check` | 打印 32 点对照表（**人眼**核对用） |
| `--dump-tsv` | 打印 TSV（**机器**比对用，给 T40） |
| `--check-pymodbus` | 只探测依赖是否可用（给构建脚本决定 skip/fail） |

### 4.3 读真机 / 读模拟器

```bat
13\build\modbus_probe.exe --plan                  :: 只打印分块读计划（不发报文）
13\build\modbus_probe.exe --port 15020            :: 读一遍 32 点
13\build\modbus_probe.exe --port 15020 --repeat 10 --interval 1
13\build\modbus_probe.exe --host 192.168.1.10 --port 502 --unit 3
13\build\modbus_probe.exe --port 15020 --set-power -60 --verbose
```

`ok` 列是**最重要的一列**：`--` 表示该点本拍不可信。
记住 §2.1 —— 值为 0 且 `ok=false` 与"真的是 0"，在协议上完全一样，
**只有这一列能把它们分开**。

---

## 5. ★ 关口功率口径（跨语言可复核的证据）

Python 从站的电表读数 = 平衡值 + `--bias`：

```
p_grid = (p_load + standby) - p_pv - p_bat + meter_bias
```

主站若在本地重算 `(load + standby - pv - p_bat)`，会得到比电表**小 bias** 的值。
两个写法在 `bias = 0` 时**逐位相等** —— 所以必须用非 0 bias 把两者拉开。

实测（`--bias 40`，读两遍，第二遍已下发 -60 kW）：

| 遍 | `p_bat` | `p_grid` | 校验 |
|---|---|---|---|
| 1 | 0.00 | 272.00 | `380+2-150-0+40 = 272` ✓ |
| 2 | -57.41 | 329.41 | `272 - (-57.41) = 329.41` ✓ 且模型正在跟踪指令 |

T42 直接断言 `p_grid - 本地重算 == 40`，并带**反向守卫**
（`|差| > 30`）—— 没有反向守卫，两个口径碰巧相等时断言也会过。

---

## 6. 测试结构（三层，职责不重叠）

| 层 | 文件 | 断言 | 依赖 | 保证什么 |
|---|---|---|---|---|
| 协议层 | `tests/test_modbus_tcp.cpp` | **221** | 无（内置 fake 从站） | 客户端符合协议 + 异常路径正确 |
| 适配器契约 | `tests/test_modbus_device_io.cpp` | **133** | 无（内置 fake 从站） | 32 点快照 / 原子下发 / 部分成功 / 安全位 |
| 跨语言联调 | `tests/test_modbus_bridge.cpp` | **83** | **Python + pymodbus** | 两个**独立实现**之间能真正互通 |
| | **合计** | **437** | | |

### ★ 为什么必须有第三层

**自己写两端等于自己和自己对答案。** 前两层的 fake 从站也是我们写的 ——
双方共同误解协议时谁都发现不了（比如都把 32 位字序记成 AB，那前两层全绿，
现场接真机全错）。`pymodbus` 是业界事实标准，拿它当对端，
"我们的客户端符合协议"才第一次成为一个**有外部证据**的结论。

这一层立刻抓出了 6 个真问题（见 §8），其中 3 个是前两层**照不出来**的。

### 环境缺失时的行为（**skip 而非 fail**，但必须显式）

`test_modbus_bridge.exe` 做**两级**环境探测：

| 级别 | 探测 | 失败含义 | 处置 |
|---|---|---|---|
| 1 | `--dump-tsv`（不 import pymodbus） | 没有 python / 脚本路径不对 | SKIP |
| 2 | `--check-pymodbus` | 有 python 但**没装 pymodbus** | SKIP + 安装指引 |
| — | 两级都过 | 起不了服务 = **真缺陷** | FAIL |

★ 第 2 级不能省：`--dump-tsv` 不需要 pymodbus，所以"没装 pymodbus"
会通过第 1 级，然后在起服务时失败 —— 被报成**代码缺陷**。
这个区分不做，构建脚本就会在没装 pymodbus 的机器上**永久假红**。

解释器定位顺序：`EMS_PYTHON` → `13/.venv/Scripts/python.exe` → PATH 上的 `python`。
（`build_test.bat` 与 C++ 侧的 `which_python()` 是同一套顺序。）

### ★ 但"测试层诚实"只做了一半 —— 上层必须也承认自己没跑

第一版只在**测试层**做对了（打 `SKIPPED=7`、退出码 0），跑完整 `build_all.bat` 才发现
`build_test.bat` 的收尾行**仍然印 `[OK] 13\ Modbus 三层测试全部通过`** ——
**最终结论行是假的**。这正是本项目最忌讳的「静默降级」：把"没验"写成了"验过"。

三处修法（完整复盘见 `CHANGES.md` §27.12）：

1. 桥接测试**总是**打印 `SKIPPED=n`（哪怕是 0）—— 让"跑了"有**正面证据**；
   并另落一行**纯 ASCII** 的 `build/bridge_status.txt`（`PASS=… FAIL=… SKIPPED=…`）。
2. `build_test.bat` 先 `del` 旧状态文件 → 跑测试 → `findstr /C:"SKIPPED=0"` 取判定 →
   收尾行**据实**二选一：`[OK] 三层（437 条）` / `[SKIP] 前两层（221+133），跨语言层未运行，
   本次全量口径 8906`。文件不存在也按「未跑」处理 —— **宁可多报 SKIP，不可漏报**。
3. 为什么读**文件**而不是 grep 输出：`build_all.log` 是**混合编码**的
   （`.bat` 的 `echo` 走 CP936、C++ 的 `printf` 走 UTF-8），对中文做 grep 不稳。

> ★ 通用纪律：**凡是"允许不跑"的测试层，都必须有一个"跑了没有"的显式信号，
> 一路传到最外层结论行。** 只让测试层自己诚实是不够的 ——
> 只要上层给出一个相反的结论行，读结论的人就只会记住那一行。

### 判据有效性反证（项目最硬纪律，本模块做了 **5 次**）

| # | 退回/破坏的写法 | 结果 | 抓它的测试 |
|---|---|---|---|
| 1 | Python 侧 `p_grid` 去掉 `+ meter_bias` | **3 条红** | T42 |
| 2 | Python 点表 `IR_P_GRID` 6 → 7 | **3 条红**（T40 精确定位到第 3 行并并排打两侧字段） | T40 |
| 3 | Python 侧 `read_command` 恒返回 None（不消费指令） | **4 条红** | T43 / T44 |
| 4 | `simdata` 交换 DI↔IR | pymodbus 构造时 `TypeError` 直接拒绝 | ★ **非缺陷**，见下 |
| 5 | `simdata` 交换 CO↔DI（同为 BITS） | 仍 83/0 全绿 | ★ **非缺陷**，见下 |

★ 反证 4/5 的意义：**它们证明我在注释里写错了一句**。原注释声称
"元组顺序写反会让四张表整体错位、只能靠跨语言对账抓"。实测结论是：
① 跨类型写反由 pymodbus 的类型检查当场拒绝，不静默；
② 同类型写反**没有影响** —— 表身份由**位置**决定，而读写都走同一功能码入口，
换掉某个位置上的对象不改变对外语义。
真正的风险是 **C++ 侧 `Table` 枚举与标准功能码的对应**写错，那才"读错表"且协议层完全合法，
由 T40 + T41 守着。**注释已按实测结论改正。**
（这就是"注释声称的行为必须与代码实际行为一致"那条纪律的又一次实战。）

---

## 7. 目录结构

```
13/
├─ src/
│   ├─ modbus_tcp_client.h     协议层：MBAP/PDU 编解码、事务、超时、串包自愈
│   │                          ★ 纯函数 pdu:: 命名空间，不碰 socket —— 便于字节级断言
│   ├─ modbus_point_map.h      ★ 映射的唯一真相源 + 4 条编译期守卫 + 分块表
│   ├─ modbus_device_io.h      IDeviceIO 第四个实现（现场那个）
│   ├─ fake_modbus_slave.h     进程内测试从站，**可注入故障**（半包/串包/异常/静默）
│   │                          ★ 它同时是"可执行的协议文档"
│   └─ main_probe.cpp          现场点表核对工具
├─ tests/
│   ├─ test_modbus_tcp.cpp      T01~T15  协议层（221）
│   ├─ test_modbus_device_io.cpp T21~T30 适配器契约（133）
│   └─ test_modbus_bridge.cpp   T40~T47  跨语言联调（83）
├─ sim/
│   ├─ modbus_slave.py         Python 从站（pymodbus）+ 设备模型 + 32 点 TSV 导出
│   └─ requirements.txt        只依赖 pymodbus
├─ scripts/
│   ├─ build.bat               → modbus_probe.exe
│   ├─ build_test.bat          → 三层测试
│   └─ run_sim.bat             → 起 Python 从站
├─ build/                      产物（.exe / .o）**+ 运行期产物**
│                              bridge_slave.log（Python 从站的 stdout/stderr）
│                              bridge_status.txt（纯 ASCII 状态行，供 .bat 判定第三层跑没跑）
│                              ★ 运行期产物落这里，不落模块根（可用 EMS_BRIDGE_LOG /
│                                EMS_BRIDGE_STATUS 覆盖）
├─ docs/README.md              本文
└─ docs/design.md              设计推演
```

---

## 8. 已知限制与遗留

### 8.1 本模块**没有**做的（下一步的候选）

| 缺口 | 影响 | 备注 |
|---|---|---|
| **只有 TCP** | 现场大量设备是 RTU（RS-485） | 协议层已把"帧同步"与"字节流"分开，加 RTU 主要是补 CRC16 + 静默间隔 |
| **EXT 已落地、尚未接线** | 调度遥调**仍落不到权限区间** | **A3.1 已交付**（EXT 点区 + 窄接口 + 网关落点已通）；剩 **A3.2**：`narrow_interval_by_ext()` 接进 `05/` 第 10 条约束 + 装配层 |
| **设备全点表不全** | 段里只有 EMS 抽象表（40 点；本模块绑设备侧 32 点） | 规划里的 **B2** |
| **电表无漂移/时延模型** | 模拟电表比真表"干净" | Python 侧只有静态 `--bias` |
| **单连接顺序请求** | 高点数时吞吐低 | 现场 EMS 通常够用；真要并发需连接池 |

### 8.2 环境依赖（**会改变断言总数**）

跨语言层需要外部 Python + pymodbus。因此全量基线有**两个合法值**：

| 条件 | 13/ 贡献 | 全量基线 | `build_test.bat` 收尾行 |
|---|---|---|---|
| **开箱默认**（仓库里没有装了 pymodbus 的 Python） | 359 | **8906** | `[SKIP] … 跨语言层未运行` |
| 有 pymodbus（`13\.venv` 或 `EMS_PYTHON`） | 437 | **8984** | `[OK] … 三层测试全部通过` |

★ **默认口径是 8906，不是 8984** —— 这一点是跑完整 `build_all.bat` 才发现的：
pymodbus 装在哪台解释器上，**构建脚本管不着**（本项目脚本不依赖任何个人/工具链环境）。
报告里必须写清用的是哪个口径，否则"数字对不上"会被误判成缺陷。

**判据**：`grep "SKIPPED=0" build_all.log` 命中 → 8984 口径；命中 `SKIPPED=7` → 8906。

★ 桥接测试**总是**打印 `SKIPPED=n`（哪怕是 0）。为什么：只在 skip 时才打印的话，
"第三层确实跑了"在日志里就**没有正面证据**，只能靠"没有那一行"去推断 ——
而"缺行 / grep 写错 / 日志被截断"三者不可区分。**正面证据优于"没有反面证据"。**

它还另落一行**纯 ASCII** 的 `build/bridge_status.txt`（`PASS=… FAIL=… SKIPPED=…`），
供 `build_test.bat` 用 `findstr /C:"SKIPPED=0"` 取判定 —— 读文件而不是 grep 中文输出，
是因为 `build_all.log` 是**混合编码**的（`.bat` 的 `echo` 走 CP936、C++ 的 `printf` 走 UTF-8），
对中文做 grep 不稳。

建环境（两步，之后就是 8984 口径）：

```bat
python -m venv 13\.venv
13\.venv\Scripts\pip install -r 13\sim\requirements.txt
```

`build_test.bat` 会自动捡起 `13\.venv`；也可以显式 `set EMS_PYTHON=<...>\python.exe`。

### 8.3 与 pymodbus 版本绑定的硬事实

`13/sim/modbus_slave.py` 文件头有完整记录。三条最容易踩的：

1. **不要走 `ModbusDeviceContext` + `ModbusSequentialDataBlock` 老路**：
   3.15 里 `ModbusDeviceContext.__init__` 对 data block 做 `deepcopy`，
   构造后再改 `ir.simdata[...]` **服务端看不见**，而且**不报错**（主站永远读到 0）。
   正确做法：`pymodbus.simulator` 的 `SimData/SimDevice` + `pymodbus.server.ModbusTcpServer`。
2. **`SimData.count` 对 BITS 是乘数**：`count=64` 配 64 个 bool 会展开成 4096 位。传 1。
3. **更新寄存器只有一个公开入口**：`server.async_setValues(unit, fc, addr, values)`，
   `fc` 是**标准功能码**（1=CO 2=DI 3=HR 4=IR）。所以脚本必须自己持有
   `ModbusTcpServer` 实例 —— `StartAsyncTcpServer()` 是 fire-and-forget，拿不到句柄。

---

## 9. 与相邻模块的关系

| 模块 | 关系 |
|---|---|
| `04/src/device_io.h` | 提供 `IDeviceIO` / `DeviceLimits` / `DeviceActuals`；本模块是它的第 4 个实现 |
| `04/src/data_models.h` | `RealtimeSnapshot` / `PowerCommand`（**快照语义由这里定义**，不是本模块） |
| `05/` | 消费 `read_limits()` 的输出的那两个 bool（BMS 禁充放）折进安全区间 |
| `07/` | 实时闭环；`ModbusDeviceIO` 与 `SimDeviceIO` 可直接互换装配 |
| `07/src/rtdb/ems_point_table.h` | **点表真相源（40 点）**。本模块只绑设备侧前 32 点（EXT 区不经设备总线），点名与索引直接取自它，不另抄一份 |
| `11/` | 跨进程联调（RT_DB 路线）。本模块是"跨**网络**"那条路，与它并列 |
| `12/` | 验收；本模块的断言总数进全量基线 |
| `P3/` | IEC104 从站网关（对上）。本模块是**对下**（对设备）。两条方向相反，都不得绕过 `05/` |

★ **共同纪律**：无论数据从哪来（IEC104 遥控 / Modbus 写 / RT_DB 写），
**调度侧的任何设定都不得绕过 `05/` 安全约束引擎**。本模块的 `write_command()`
只接受 `PowerCommand`（已含 `p_upper/p_lower`），不提供"只写功率"的接口 ——
这就是在类型上表达这条纪律。
