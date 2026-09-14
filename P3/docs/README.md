# 产品化 P3：通信层（Modbus 设备侧 / IEC104 调度侧 / TCP 传输层）

> 一句话：P0 定义了 `IDeviceIO` 抽象，P0.5 用进程内点表证明了"可换"，RT_DB 跨进程证明了"能换到实时库"，
> **P3 把这条线接到现场真实介质上 —— 设备侧 Modbus，调度侧 IEC 60870-5-104；
> 并用真内核 socket 的 TCP 传输层，把"环回验证"补成"能接真机"。算法层一行不改。**

---

## 1. 为什么需要 P3

P0 ~ RT_DB 走完以后，`IDeviceIO` 上已经并列了三个适配器：

| 适配器 | 介质 | 验证了什么 |
| --- | --- | --- |
| `SimDeviceIO` | 进程内 `PlantModel` | 接口能跑闭环 |
| `MemoryDeviceIO` | 进程内点表 `unordered_map` | 换数据源**不改算法**（进程内） |
| `RtDbDeviceIO` | 共享内存实时库 | 换数据源**跨进程**仍然不改算法 |

但这三个都还在**一个进程 / 一个地址空间**里。现场要接的是两根真实线缆：

- **设备侧（南向）**：PCS / BMS / 电表挂 **Modbus RTU/TCP**，EMS 是 **主站**，要周期性读遥测、下发指令；
- **调度侧（北向）**：上级调度 / 云端走 **IEC 60870-5-104**，EMS 在这里是**从站（受控站）**，要响应总召唤、上送遥测、接收设定值。

这两侧一旦直接写进 `EmsRuntime`，P0 立的规矩就全废了 —— 算法会第一次看见"寄存器地址""IOA""帧序号"这些介质概念。
**P3 的目标就是：把 Modbus 与 IEC104 做成两个新的 `IDeviceIO` 适配器，让它们与前三个并列，算法层继续零改动。**

---

## 2. 交付物

```
P3/
├── src/
│   ├── modbus_codec.h           Modbus 帧编解码纯函数（MBAP / PDU / CRC16 / 字序）+ 常量
│   ├── modbus_device_io.h       ModbusDeviceIO（IDeviceIO 适配器）+ 寄存器映射 + IModbusTransport
│   ├── modbus_slave_sim.h       ModbusSlaveSim（从站仿真）+ LoopbackModbusTransport（环回装置）
│   ├── iec104_codec.h           IEC104 帧/ASDU 编解码纯函数（APCI 三帧型 / 8 类 ASDU / 小端）
│   ├── iec104_device_io.h       Iec104DeviceIO（IDeviceIO 适配器）+ IOA 映射 + 受控站仿真 + 环回
│   ├── tcp_transport.h          ★ 真 socket 传输层：ModbusTcpTransport / Iec104TcpTransport
│   └── main.cpp                 场景 F/G/H：Modbus 演示 / IEC104 演示 / 四介质逐拍比对
├── tests/
│   ├── test_modbus.cpp          T31~T36（622 断言）
│   ├── test_iec104.cpp          T41~T46（646 断言）
│   └── test_tcp.cpp             T51~T57（87 断言，独立线程 + 真 listen/accept/recv/send）
├── scripts/
│   ├── build.bat                编译演示 ems_comms.exe
│   ├── run_demo.bat             编译 + 跑演示
│   ├── build_test_modbus.bat    编译 + 跑 test_modbus.exe
│   ├── build_test_iec104.bat    编译 + 跑 test_iec104.exe
│   └── build_test_tcp.bat       编译 + 跑 test_tcp.exe（MinGW 需 -lws2_32）
├── docs/
│   ├── README.md                本文件：为什么 / 交付物 / 验证 / 现场约束
│   └── design.md                设计细节：帧层 / 映射契约 / 会话时序 / 等价性方法学
└── build/                       编译产物（git ignore）
```

构建产物：

```
P3\build\ems_comms.exe          产品化 P3 演示：Modbus / IEC104 / 四介质逐拍比对
P3\build\test_modbus.exe        P3/ Modbus 单元测试（T31~T36 / 622 断言）
P3\build\test_iec104.exe        P3/ IEC104 单元测试（T41~T46 / 646 断言）
P3\build\test_tcp.exe           P3/ TCP 传输层单元测试（T51~T57 / 87 断言 / 真 socket）
```

---

## 3. 两个适配器的边界（一句话版）

| | ModbusDeviceIO | Iec104DeviceIO |
| --- | --- | --- |
| 角色 | EMS 是**主站**（Master） | EMS 是**从站**（受控站 / Controlled Station） |
| 对端 | PCS / BMS / 电表 | 上级调度 / 云端 SCADA |
| 传输模型 | **请求-响应**（`transact`） | **字节流**（`send` / `receive`，TCP 会粘包） |
| 字节序 | **大端**（Modbus 规定） | **小端**（IEC104 规定，除个别字段） |
| 上送方式 | EMS 主动轮询读 | 从站自定时上送 + EMS 用 TESTFR/S 帧当拍点 |
| 写指令 | 功能码 `0x10` 写 CMD 寄存器块 | `C_SE_NC_1`(50) 设定值 + `C_SC/C_DC` 遥控 |
| 首次握手 | 无（直连） | `STARTDT` + **总召唤**（GI 三段式） |
| 周期槽点 | `poll()` 即一轮读写 | `poll()` = 心跳 + 窗口确认 + 收帧 |

两者的共同点，也是 P3 最想强调的：**它们对外都只是 `IDeviceIO`**。
`EmsRuntime` 装配时拿到的指针类型完全一样，`07/tests/test_realtime_loop.cpp` 的算法路径一行没动。

---

## 4. 怎么验证：环回传输 + 闭环逐位等价

### 4.1 环回装置 —— 跳过内核，不跳过字节

没有真实 PCS / 调度主站，怎么"真实验证"？P3 的做法是**只跳过内核 socket，不跳过任何字节编解码**：

```
   EmsRuntime
       │  IDeviceIO
       ▼
  ModbusDeviceIO ──► 编码成 MBAP+PDU 字节 ──► LoopbackModbusTransport ──► ModbusSlaveSim
                                                                              │
  ModbusDeviceIO ◄── 解析响应字节       ◄── 环回 ◄────────────────────────────┘
```

`LoopbackModbusTransport::transact()` 收下请求字节，直接把 `slave_->handle()` 产出的响应字节回灌 ——
**中间没有一次"函数直调"**，从 `build_read_request()` 到 `parse_response()` 全是现场会用到的同一份代码。
IEC104 侧同理：`LoopbackIec104Transport` 只做字节搬运，`Iec104ControlledStationSim` 走完整的 APCI/ASDU 编解码。

这样做的意义：如果等价性只靠"直接调从站函数"来验，测过的就不是要跑的；环回把内核那一跳省掉，
**留下的是真正会出错的那部分（帧格式、字序、序号状态机、地址映射）**。

### 4.2 等价性范式 —— 同一装配序列，两路逐拍比对

沿用 RT_DB 的 T26 范式：

```
环境脚本（装配序列）完全相同
   ├─► 基准路：MemoryDeviceIO        直连进程内点表
   └─► 被测路：ModbusDeviceIO  经 MBAP+PDU 报文（或 Iec104DeviceIO 经 APCI+ASDU 报文）
400 拍 StepRecord 逐拍比对：p_cmd / p_actual / p_grid / soc / 区间 / 状态 / reason
```

### 4.3 量化边界分离 —— 两件事分开验

这里有个必须讲清的坑：**"代码写错"和"精度不够"是两件事**，混在一起验就分不清。

| 通道 | Modbus | IEC104 | 用途 |
| --- | --- | --- | --- |
| **宽精度** | `kFloat64BE`（4 寄存器，f64） | `M_ME_WIDE`(200) 私有 double | 验**逐位等价**（diff=0，证明编解码无损） |
| **现场标准** | `kFloat32BE`(ABCD) / `kFloat32Swap`(CDAB) | `M_ME_NC_1`(13) 短浮点 | 验**量化偏差量级**与决策拓扑不变 |

结论（实测）：

- 宽精度通道：**400 拍逐位 diff = 0**；
- 现场标准通道：**决策拓扑差异 = 0 拍**，`|Δcmd|max = 3.93681e-05 kW`，`|ΔSOC| = 1.08913e-08`。

两路是**独立实现**（一个走 Modbus 字序，一个走 IEC104 小端 ASDU），却推出**同一个偏差上界 ≈ 3.9e-05 kW** ——
互为旁证，说明这个数不是某个实现的偶然，而是 f32 在 100 kW 量程下的固有分辨率。

### 4.4 第三级：真内核 socket —— 环回的承诺兑现

环回装置刻意跳过内核。**"能接真机"这句话必须由真 socket 来兑现**，于是有了 `tcp_transport.h`：

```
   EmsRuntime
       │  IDeviceIO
       ▼
  ModbusDeviceIO ──► IModbusTransport ──► ModbusTcpTransport  ═══╗
                                                        真内核  ║  127.0.0.1
                                                        socket  ║  内核协议栈
  Iec104DeviceIO ──► IIec104Transport ──► Iec104TcpTransport  ══╝
                                                        （服务端在独立线程 listen/accept）
```

**换掉传输层，适配器与算法一行不改** —— 这正是 P0 那条承诺链条的完整兑现：

```
SimDeviceIO → MemoryDeviceIO → RtDbDeviceIO → Loopback → TCP
```

TCP 层有三个现场踩出来的实现纪律，都写进了 `tcp_transport.h` 的注释：

1. **Modbus 必须按 MBAP 的 `Length` 字段分帧**，不能"发一次收一次"。TCP 是字节流，
   一次 `recv` 可能只回来半个响应，也可能把两个响应粘在一起。做法是先精确读 7 字节 MBAP，
   再按 `Length` 精确读剩余部分；事务号（TID）回显不一致直接拒收（防错位 / 串话）。
2. **IEC104 的 `receive` 必须是"三态"语义**：`false` = 链路错误 / `true & len==0` = 这一轮没报文 /
   `true & len>0` = 收到 N 字节。适配器的 `drain_rx()` 靠这三态区分"对端哑了"和"没数据"。
3. **`connect` 要带超时**。阻塞式 connect 连不通的地址会挂 20 秒以上，现场表现为"EMS 卡死"，
   比连不上更难查。用非阻塞 connect + `select`。

还多了一个**只有跨线程 + 跨 socket 才会出现**的坑，值得单独记下来：

> 104 是"服务端主动上送"，上送值是**服务端处理某一帧时设备的当前值**。
> 若服务端还在消化上一拍遗留的帧，它读到的就是上一拍的环境 —— 上送的字节本身没错、
> 数值也全对，但会在客户端已经写入本拍环境之后才被读走，表现为**整体滞后一拍**。
> 更隐蔽的是 `EMS_P_GRID`：它不是外部注入量，而是 `MemoryDeviceIO::execute()` 里
> `P_LOAD + 站用电 − P_PV − P_BAT` 现算出来的**派生点**，而 `read_actuals()` 读的是
> **最近一次上送的 cache** —— 环回装置下 `read_actuals()` 直接读设备，所以完全看不到这个问题。
>
> 解决办法不是"加等待"，而是**汇合（rendezvous）+ 核对**：
> 服务端每消化完一批入向字节把 `round` 加一，客户端据此确认"我发出去的帧已被消化完"，
> 并在改环境前先把这些旧值上送**丢弃**；改完环境再触发，之后逐项核对 cache 是否追上设备。
> 见 `tests/test_tcp.cpp` 的 `Iec104TcpRig::sync_uplink()` / `refresh_after_execute()`。

实测（400 拍，真 socket）：

| 路径 | 结果 |
| --- | --- |
| Modbus over TCP | **逐位差异 0 拍**，事务 2801 次，峰值指令 100 kW |
| IEC104 over TCP | **逐位差异 0 拍**（`kWidePrivate`），链路错误 0 |
| TCP 分片（服务端每次只发 2 字节） | 客户端自行重组，采集失败 0 |
| 事务号错位（强制响应 TID 改为 0x0001） | 被拒绝，`format_errors==1`，且**不被误判为断链** |
| 真断链（服务端处理 800 帧后关闭） | 采集不可信 → FAULT → 指令归零、门控 |

"强制错位仍在线""断链后门控归零"这两条是**反向守卫**：避免用"两边都恒零"骗过等价性。

---

## 5. 故障语义与 RT_DB 对齐

P3 不是新造一套故障语义，而是**复刻 RT_DB 已经定下的口径**：

| 现场事件 | 链路表现 | 算法层看到 |
| --- | --- | --- |
| 链路静默 / 全部数据无效 | Modbus 从站沉默 / IEC104 t3 超时 | `data_valid=false` → 状态机 FAULT → 指令归零 |
| 单点采集失败 | 一帧超时或异常响应 | **保留最近有效值**，`quality_ok_=false`，不把异常当数据 |
| 心跳丢但遥测到 | IEC104 只在 keepalive 丢 | HOLD_LAST：冻结上一拍指令，**权限区间钉成单点**（禁动作） |
| 设备故障（PCS_FAULT） | 经寄存器/ASDU 上送 STA 位 | FAULT → 门控归零 → **撤销许可** → 恢复后停在 READY **不自动带载** |

"恢复不自动带载"是安全要求：故障消失只回到 READY，必须由上层重新给指令才动。

---

## 6. 现场会用到的三个约束

1. **方向纪律由介质侧强制**：Modbus 从站仿真里 `is_writable_reg()` 只放行 CMD 区（`0x0100`），
   写 MEAS/CFG/STA 一律回异常码 `0x02`；IEC104 侧只接受 `C_SE_NC_1` 写 CMD。
   **越界写在介质层就被拒**，不靠"调用方自觉"。
2. **`execute()` 返回值不可信**：写成功只是"报文发出去了"，闭环一律在**下一拍 `read_snapshot()`** 里确认。
   采集失败时保留旧值，绝不用过期值伪装成新值。
3. **无权限区间时只写单点**：Modbus 的整块 `0x10` 会把 CMD 区三个点一起写。若权限区间退化成单点，
   `execute()` 改走 `write_point(EMS_CMD_P_BAT, ...)` —— 因为**多发一个 0/0 的上下限，对端若按权限区间执行会理解成禁止动作**。

---

## 7. 测试清单

### Modbus（T31~T36，622 断言）

| 用例 | 证明什么 |
| --- | --- |
| **T31** | 帧逐字节：CRC16 自检值 `0x4B37`、字序 ABCD vs CDAB、f64 位级恒等、MBAP 自洽、异常响应解析 |
| **T32** | 异常与采集失败：异常不当数据、读失败保留旧值、非法地址/单元号/未知功能码分别回对应异常码 |
| **T33** | 映射契约：f32 共 **780** 寄存器 / f64 共 **792** 寄存器；点名 / 块 / 地址与真相源逐字一致 |
| **T34** | 闭环等价：f64 路径 **diff=0**；f32 路径决策拓扑差异 0 拍、Δcmd 最大偏差 **3.93681e-05 kW** |
| **T35** | 断链与自愈：**模式 A** 链路静默 → FAULT；**模式 B** 心跳丢 → HOLD_LAST 冻结在 **4.28963 kW**（第 130 拍起） |
| **T36** | 故障经寄存器驱动状态机：`PCS_FAULT` → FAULT → 门控归零 → 恢复不自动带载；设备侧读回指令 **5.8891 kW** |

### IEC104（T41~T46，646 断言）

| 用例 | 证明什么 |
| --- | --- |
| **T41** | APCI 逐字节：U/S/I 三帧型、N(S)/N(R) 15bit 序号、LEN 上限 253、粘包边界（返回 0 = 半个帧） |
| **T42** | ASDU 逐字节：类型 1/13/45/46/50/70/100/200 的**小端**布局、品质位、`M_ME_WIDE` 位级无损、长度语义 |
| **T43** | 会话时序：STARTDT 确认、总召唤 I 帧数=1、心跳 14/14、S 帧=30、最大未确认=0、k=12；CA 不匹配丢弃；未知类型与畸形分开计数；STOPDT 处理 |
| **T44** | 闭环等价：`kWidePrivate` **diff=0**；`kStandard` Δcmd 最大偏差 **3.93681e-05 kW** |
| **T45** | 通信中断：链路断 → FAULT；对端哑 t3 → FAULT；自愈；**序号不回退** |
| **T46** | 映射契约：IOA 分段（`0x4001` 遥测 / `0x4101` 参数 / `0x4201` 状态 / `0x4301` 遥控）、全局唯一、分辨率事实 |

### TCP 传输层（T51~T57，87 断言，真 socket）

服务端是**独立线程 + 真 `listen`/`accept`/`recv`/`send`**，数据真的经过内核协议栈与 127.0.0.1 网卡回路，
没有跳过任何一个字节的编解码，也没有跳过内核。

| 用例 | 证明什么 |
| --- | --- |
| **T51** | TCP 基座：连接成功 / **连接失败在超时预算内返回**（已关闭端口 510 ms < 2000 ms）/ `receive` 三态语义（超时=0 / 对端关闭=-1） |
| **T52** | Modbus TCP 事务：往返字节与环回实现一致；服务端捕获的**每一帧** MBAP 自洽（PID=0、UnitId=1、功能码合法）、出现过 MEAS 块读；SOC 与设备侧一致 |
| **T53** | 分帧鲁棒性：**服务端每次只发 2 字节**时客户端自行重组（采集失败 0）；**强制事务号错位**必须被拒（`format_errors==1`）且不误判断链 |
| **T54** | Modbus TCP 闭环等价：400 拍**逐位差异 0 拍**，峰值 100 kW，SOC 0.5→0.499318，事务 2801 |
| **T55** | IEC104 TCP 建链：STARTDT 确认 / 初始化结束(`M_EI_NA_1`) / 总召唤三段式跨 socket 成立；总召回数=1、未确认帧=0 |
| **T56** | IEC104 TCP 闭环等价：400 拍**逐位差异 0 拍**；`resync_failures==0`、`exec_refresh_failures==0`（把"滞后一拍"这类时序假象钉死） |
| **T57** | 真断链：断链前峰值 100 kW（证明不是"恒零骗等价"）→ 服务端关闭 → 采集不可信 → **FAULT** → 指令归零、门控 |

---

## 8. 怎么跑

```bat
cd P3
scripts\build_test_modbus.bat      :: 期望 PASS=622 FAIL=0 / ALL TESTS PASSED
scripts\build_test_iec104.bat      :: 期望 PASS=646 FAIL=0 / ALL TESTS PASSED
scripts\build_test_tcp.bat         :: 期望 PASS=87  FAIL=0 / ALL TESTS PASSED（真 socket）
scripts\run_demo.bat               :: 场景 F/G/H 演示（含四介质逐拍比对表）
```

一键全量（含 P3 三步）：

```bat
scripts\build_all.bat              :: 期望 [BUILD ALL OK] All 26 components built.
```

> `build_test_tcp.bat` 是**纯 ASCII + CRLF** 的（含括号的路径在部分 shell 下会被转义吞掉，
> 脚本内一律用相对路径）。MinGW 链接需要 `-lws2_32`。

---

## 9. 关键认识

1. **"可换性"要在真实编码路径上证明，而不是在函数调用上证明。** 环回只跳过内核一跳，
   留下的恰恰是最容易出错的部分。P0.5 证明了接口能换，P3 证明了换成**真实报文**也能换。
2. **等价性与量化必须分开验。** 用宽精度私有通道证"零误差"，用现场标准通道量"误差多大"——
   否则一旦 f32 有偏差，无法判断是字序写错还是精度天花板。
3. **两条独立实现给出同一个偏差上界（≈3.9e-05 kW），比单条实现的"通过了"更有说服力。**
4. **故障语义是复用的，不是重新设计的。** P3 全部沿用 RT_DB 的 `data_valid` / HOLD_LAST / 恢复不自动带载口径，
   介质换了、安全性没换 —— 这也是"算法零改动"的隐藏收益。
5. **协议差异是真实存在的，抽象要留足空间。** Modbus 是请求-响应、大端；IEC104 是字节流、小端、有会话状态机。
   把它们硬塞进同一个"读写函数"形状会漏掉粘包和序号回退，所以 `IModbusTransport` 与 `IIec104Transport` 形状不同。
6. **"数值全对但整体错一拍"要先怀疑装配，不要先怀疑传输层。** TCP 路的最后一跳暴露过一次：
   逐拍比对整体滞后一拍、数值一个不差。根因不在 socket 也不在编解码，而在
   **服务端上送时刻读到的设备值与客户端取值时刻不匹配**，叠加 `EMS_P_GRID` 这种
   "由 `execute()` 派生、却用最近一次上送的 cache 读数"的字段。处理原则：
   **能汇合就别猜、能核对就别等** —— 传真的纪律比调大超时更可靠。

---

## 10. 局限与后续

- **已接真实内核 socket（P3 尾巴）**：`tcp_transport.h` 提供 `ModbusTcpTransport` /
  `Iec104TcpTransport`，真 `connect`/`send`/`recv`，Windows(Winsock2) / POSIX 双实现。
  **接真机时只需换 `set_endpoint(host, port)`，适配器与算法一行不改。**
- **现场参数仍需标定**：`set_timeout_ms`（Modbus 事务超时）、`set_min_wait_ms`（104 建链确认窗口）、
  `set_expect_unit_id`（防串话到总线上别的从站）、`set_stale_after_polls`（104 的 t3 判据）——
  这些是**现场整定项**，默认值是验收环境下的取值，不代表现场最优。
- **未做冗余双网 / 时钟同步**：现场常要求主备双链路与 SNTP 对时，属于调度接入的配套，尚未纳入。
- **未用 lib60870**：IEC104 的 APCI/ASDU 全部自研（约 1.9k 行），避开 GPLv3 / 商业双授权问题，
  也让 T41/T42 能做真正的逐字节断言。
- **点表规模**：当前 30 点（MEAS 7 / CMD 3 / CFG 14 / STA 6）。扩容只需改 `07/src/rtdb/ems_point_table.h`
  这一处真相源，两个映射表会自动跟随（`static_assert` 守着点数一致）。
- **TLS / 认证未纳入**：现场若要求 104 走加密隧道（如 IEC 62351），需在传输层之上再加一层，
  当前 `tcp_transport.h` 只做传输层。
