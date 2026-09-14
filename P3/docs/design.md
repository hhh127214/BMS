# P3 通信层设计

> 本文说明 P3 两个适配器的**设计细节**：帧层怎么编、映射怎么建、会话怎么走、等价性怎么证。
> 阅读顺序建议：§1 目标 → §2 分层 → §3 Modbus → §4 IEC104 → §5 与 IDeviceIO 的对应 → §6 验证方法学。

---

## 1. 设计目标与硬约束

| 目标 | 落地手段 |
| --- | --- |
| 算法层零改动 | 两者都实现 `IDeviceIO`，`EmsRuntime` 拿到的指针类型不变 |
| 无硬件可做真实验证 | `LoopbackTransport` 只跳过内核 socket，不跳过字节编解码 |
| 可证明"换介质不改结果" | 同一装配序列，400 拍 `StepRecord` 逐拍比对 |
| 不引入第三方协议栈 | Modbus / IEC104 全部自研纯函数编解码，逐字节可断言 |
| 复用既有故障语义 | 全部对齐 RT_DB 的 `data_valid` / HOLD_LAST / 恢复不自动带载 |
| 杜绝"点名漂移" | 复用 `07/src/rtdb/ems_point_table.h` 作唯一真相源，`static_assert` 守点数 |

**四条不可违反的纪律**（写进代码注释并在测试里断言）：

1. `IDeviceIO` 的参数与返回一律是**业务语义结构体**，绝不出现寄存器地址 / IOA / 点名；
2. **方向分离**：设备侧只写 `MEAS/CFG/STA`，EMS 侧只写 `CMD`；越界写在介质层被拒；
3. `execute()` 返回值**不可信**，闭环一律在下一拍 `read_snapshot()` 确认；
4. 采集失败**保留最近有效值**（并置品质位），绝不用过期值伪装新值。

---

## 2. 分层

```
┌──────────────────────────────────────────────────────────────┐
│ 算法层  05/06/07/08  —— 只认识 IDeviceIO，不认识任何介质       │
└──────────────────────────────────────────────────────────────┘
                            │  IDeviceIO（8 方法）
        ┌───────────────────┴───────────────────┐
        ▼                                       ▼
┌───────────────────────┐             ┌───────────────────────┐
│ ModbusDeviceIO        │             │ Iec104DeviceIO        │
│ （南向 · 主站）        │             │ （北向 · 从站）        │
├───────────────────────┤             ├───────────────────────┤
│ ModbusRegisterMap     │             │ Iec104PointMap        │
│  30 点 → 4 寄存器块    │             │  30 点 → 4 个 IOA 段   │
├───────────────────────┤             ├───────────────────────┤
│ modbus_codec.h        │             │ iec104_codec.h        │
│  MBAP/PDU/CRC16/字序   │             │  APCI/ASDU/小端        │
├───────────────────────┤             ├───────────────────────┤
│ IModbusTransport      │             │ IIec104Transport      │
│  transact(req,resp)   │             │  send / receive       │
└───────────────────────┘             └───────────────────────┘
        │                                       │
        ▼                                       ▼
  LoopbackModbusTransport                 LoopbackIec104Transport
  ModbusTcpTransport  ← 真 socket（现场）  Iec104TcpTransport ← 真 socket（现场）
  （`src/tcp_transport.h`：只做字节进出，上面两层完全不动）
```

**为什么两个 Transport 的形状不同**：

- Modbus 是**主从请求-响应**，一次交互天然是一个"发一问、收一答"的闭环 → `transact()`；
- IEC104 是**对等字节流**，双方随时可能推数据，且 TCP 会**粘包** → `send()` + `receive()` 分开，
  由适配器自己攒帧（`parse_apdu` 收到半个帧就返回 0，等下一批字节）。

把它们统一成 `transact` 会藏掉 IEC104 的粘包与序号状态，属于抽象泄漏。

---

## 3. Modbus 设计（南向 · EMS 为主站）

### 3.1 帧层（`modbus_codec.h`）

现场用 Modbus TCP，报文 = **MBAP(7B) + PDU**：

```
 MBAP:  TransactionId(2)  ProtocolId(2)=0  Length(2)  UnitId(1)
 PDU :  FunctionCode(1) + Data
```

| 功能码 | 含义 | P3 用途 |
| --- | --- | --- |
| `0x03` | Read Holding Registers | 读 MEAS / CFG / STA |
| `0x04` | Read Input Registers | 读 MEAS（只读口径） |
| `0x06` | Write Single Register | 兜底单寄存器写 |
| `0x10` | Write Multiple Registers | 写 CMD 块（主路径） |

**异常响应**：功能码最高位置 1（`|0x80`），数据段 1 字节异常码。P3 用到的：

| 异常码 | 含义 | 触发场景（测试覆盖） |
| --- | --- | --- |
| `0x01` | Illegal Function | 收到未知功能码 |
| `0x02` | Illegal Data Address | 地址越界 / **写非 CMD 区** / 数量越界 |
| `0x03` | Illegal Data Value | 数量为 0 或超上限 |

**CRC16**（Modbus 标准多项式 `0xA001`，初值 `0xFFFF`）：实现里带自检 ——
对 `01 03 00 00 00 01` 的计算结果必须是 `0x4B37`，T31 断言这一点，防止多项式或位序写反。

**字序**（Modbus 最经典的坑）：一个值跨多个 16bit 寄存器，先后顺序有歧义。P3 支持四种：

| `RegType` | 宽度 | 字节布局 | 说明 |
| --- | --- | --- | --- |
| `kUInt16` | 1 | — | 状态字 / 计数 |
| `kInt16` | 1 | — | 有符号短整型 |
| `kUInt32BE` | 2 | — | 32bit 无符号 |
| `kFloat32BE` | 2 | ABCD | **现场标准**（大端，高字在前） |
| `kFloat32Swap` | 2 | CDAB | 部分厂家字序（低字在前、字内大端） |
| `kFloat64BE` | 4 | — | **宽精度通道**（验收用，位级无损） |

`MapProfile` 只有两档：`kStandardF32`（现场）与 `kWideF64`（验收）。**换档 = 换精度，不换代码路径**。

### 3.2 寄存器映射（`ModbusRegisterMap`）

30 个点按业务分类落到 **4 个寄存器块**，块内顺序 = 点表顺序：

| 块 | 基址 | 点数 | 内容 | 方向 |
| --- | --- | --- | --- | --- |
| 0 | `0x0000` | 7 | `MEAS.*`（P_LOAD / P_PV / SOC / P_BAT / P_GRID / SOH / ...） | 从站→主站（只读） |
| 1 | `0x0100` | 3 | `CMD.*`（P_BAT / P_LOWER / P_UPPER） | 主站→从站（可写） |
| 2 | `0x0200` | 14 | `CFG.*`（SOC 上下限 / 功率上限 / 变化率 / 效率 ...） | 只读（EMS 读设备参数） |
| 3 | `0x0300` | 6 | `STA.*`（STATE / FAULT_BITS / PCS_FAULT / BMS_FAULT / METER_LOST / ONLINE） | 只读 |

地址计算：`addr = kBlockBase[b] + 块内序号 × width`。

- `kStandardF32`（width=2）：总寄存器数 = `0x0300 + 6×2 = 780`；
- `kWideF64`（width=4）：总寄存器数 = `0x0300 + 6×4 = 792`。

T33 断言这两个数字，并逐点校验"点名 / 所属块 / 地址"与真相源一致 —— **地址算错一个点就会偏移后面全部**，所以是断言而非抽样。

`static_assert(ModbusRegisterMap::kPointCount == EMS_POINT_COUNT)` 把"点表扩容忘了改映射"变成**编译错误**。

### 3.3 `ModbusDeviceIO` 方法逐一说明

| 方法 | 实现要点 |
| --- | --- |
| `read_snapshot()` | 读块 0（MEAS），失败则**保留 `cache_` 旧值**并 `quality_ok_=false`；写入 `RealtimeSnapshot` |
| `read_limits()` | 读块 2（CFG）+ 块 3（STA），填 `DeviceLimits` |
| `read_status()` | 读块 3（STA），`data_valid()` 由"在线 + 品质 + 无致命故障位"共同决定 |
| `write_command(cmd)` | 写块 1（CMD）三寄存器（`0x10`） |
| `execute(cmd)` | 有权限区间 → `write_block()`；**退化成单点 → 只写 `CMD.P_BAT`**（见下） |
| `data_valid()` | `quality_ok_ && transport->connected()` |
| 自省 | `stale_reads()` / `timeouts()` / `exceptions()` / `writes()` / `transactions()` |

**分批规则**：单次 `0x03` 最多读 125 寄存器、`0x10` 最多写 123 —— 编解码器里做成常量
（`kMaxReadRegs` / `kMaxWriteRegs`），`read_regs()` 超限自动分批。这是 Modbus 协议的硬限制，
现场设备不遵守就会回异常。

**单点写的理由**（代码里带注释的纪律）：

```cpp
// 若权限区间退化成单点，整块 0x10 会一并下发 P_LOWER=0 / P_UPPER=0。
// 对端若按"权限区间"语义执行，会理解成「禁止动作」—— 这是危险下发。
// 因此只写 CMD.P_BAT 这一个点。
```

### 3.4 从站仿真与方向强制（`modbus_slave_sim.h`）

`ModbusSlaveSim` 内部是一块 `regs_[1024]` 寄存器文件，模拟一台 PCS/BMS：

- `publish_device_points()`：设备点表 → 寄存器（**跳过 CMD 区**，只上送设备自有量）；
- `apply_command_regs()`：CMD 区 → 设备点表（主站写下的指令被设备接收）；
- `handle()`：处理读 / 写多 / 写单；非本站 `unit_id` **沉默**（不响应，模拟总线上的其他从站）；
- `is_writable_reg()`：**只有 CMD 区（`0x0100`~）可写**，写其他区返回异常码 `0x02`。

`LoopbackModbusTransport`：`attach()` 挂从站、`inject_timeout(n)` 制造连续沉默、`inject_exception(code,n)` 注入异常响应、
`set_link_up(bool)` 模拟链路断开 —— 这四个注入点让 T32/T35 能精确构造故障。

---

## 4. IEC104 设计（北向 · EMS 为受控站）

### 4.1 APCI —— 三种帧型（`iec104_codec.h`）

IEC 60870-5-104 的链路层叫 APCI，起始字节决定帧型：

| 帧型 | 起始字节 | 结构 | 用途 |
| --- | --- | --- | --- |
| **I**（信息） | `0x00` | 含 N(S)/N(R) 序号 + ASDU | 传遥测 / 遥控 / 设定值 |
| **S**（监视） | `0x01` | 只含 N(R) | 纯确认（无数据要发时） |
| **U**（控制） | `0x03` | 1 字节功能码 | STARTDT / STOPDT / TESTFR |

U 帧功能码（`UFunc`）：`STARTDT act/con`、`STOPDT act/con`、`TESTFR act/con`。

**关键点**：

- **序号 15bit**（`& 0x7FFF`），回绕是正常的，T45 断言"序号不回退"；
- **小端**（与 Modbus 相反，除个别字段）：`put_u16_le` / `get_u24_le` / `put_f32_le` / `put_f64_le`；
- **LEN 字段上限 253**（APDU 最大 255，减去 2 字节起始 + LEN 自身）；
- **粘包**：`parse_apdu()` 若剩余字节不足一个完整帧，返回**帧长 0**，表示"半个帧，等更多字节" —— 
  T41 专门断言这个边界，T43 用分批 `feed()` 模拟 TCP 分片。

### 4.2 序号状态机与窗口

| 参数 | 值 | 含义 |
| --- | --- | --- |
| k | 12 | 最大未确认 I 帧数；满了**不发**并计 `window_stalls_` |
| w | 8 | 收到 w 个 I 帧后**必须**回一次确认 |
| t3 | 可配（`set_stale_after_polls`） | 无数据看门狗；超时清品质位、计 `stale_reads_` |

`Iec104DeviceIO::poll()` 每个周期做三件事：① 需要时发 TESTFR 心跳；② 未确认数达到 w 发 S 帧；③ 收帧并分发。
T43 断言实测：心跳 **14/14**、S 帧 **30**、最大未确认 **0**、k=**12**。

### 4.3 ASDU 类型表

| TypeId | 名称 | 方向 | P3 用途 |
| --- | --- | --- | --- |
| 1 | `M_SP_NA_1` | 上送 | 单点遥信（ONLINE / 故障位） |
| 13 | `M_ME_NC_1` | 上送 | **现场标准**遥测（短浮点） |
| 45 | `C_SC_NA_1` | 下发 | 单点遥控 |
| 46 | `C_DC_NA_1` | 下发 | 双点遥控 |
| 50 | `C_SE_NC_1` | 下发 | **设定值**（写 `CMD.P_BAT` 等 3 点） |
| 70 | `M_EI_NA_1` | 上送 | 初始化结束（STARTDT 后必发） |
| 100 | `C_IC_NA_1` | 双向 | 总召唤（GI） |
| **200** | `M_ME_WIDE`（私有） | 上送 | **宽精度通道**：双精度遥测，验收等价用 |

品质位 `Qds`：`OV`(溢出) / `BL`(被闭锁) / `SB`(被取代) / `NT`(非当前) / `IV`(无效) —— 
`IV=1` 时算法层必须按无效处理（映射到 `quality_ok_=false`）。

**长度校验语义**：`asdu_length_ok()` 对**未知类型返回 true**。这一条是 T43 踩出来的 ——
"不认识这个类型"（`unknown_type++`）与"长度字段对不上"（`malformed++`）是两件事，
混在一起会导致新增类型被误判成畸形报文。两者分开计数。

### 4.4 IOA 分段映射（`Iec104PointMap`）

IOA（信息对象地址）按业务分段，**全局唯一**：

| 段 | 基址 | 点数 | 内容 | TypeId |
| --- | --- | --- | --- | --- |
| 遥测 | `0x004001` | 7 | `MEAS.*` | 13 / 200 |
| 参数 | `0x004101` | 14 | `CFG.*` | 13 / 200 |
| 状态 | `0x004201` | 6 | `STA.*` | 1（遥信）/ 200 |
| 遥控 | `0x004301` | 3 | `CMD.*` | 50 |

IOA = `base_of_group(g) + 组内序号`。T46 断言 IOA **全局唯一**（同一地址不能属于两个点）与分段正确。

`static_assert(Iec104PointMap::kPointCount == EMS_POINT_COUNT)` 同样守住点数一致。

### 4.5 `Iec104DeviceIO` 会话流程

```
open()
  ├─ STARTDT act  ──────────►  从站回 STARTDT con
  ├─ 从站发 M_EI_NA_1（初始化结束）
  ├─ 发 总召唤 C_IC_NA_1 (QOI=20) ──► 从站三段式：ActCon → 一批 I 帧遥测 → ActTerm
  └─ gi_done_ = true

每拍 poll()
  ├─ TESTFR act（心跳节拍） ──► TESTFR con
  ├─ 未确认数 ≥ w → 发 S 帧
  ├─ 收帧：I 帧 → apply_asdu()；S 帧 → 更新对端已收；U 帧 → 分发
  └─ t3 看门狗：连续 N 拍无数据 → 清品质位 + stale_reads_++

read_snapshot() / read_limits() / read_status()
  └─ 从 apply_asdu() 落下的缓存取（**不在读的时候发帧**，避免读写耦合）

execute(cmd)
  └─ 发 3 条 C_SE_NC_1，写 CMD.P_BAT / P_LOWER / P_UPPER
```

**总召唤三段式**（现场必须严格）：调度侧发起 GI → 受控站先回 `ActCon`，再逐组上送当前值，
最后回 `ActTerm` 结束。P3 的受控站仿真完整实现这三段，T43 断言 `gi_count()==1` 且上送帧数符合。

**为什么 `poll()` 用 TESTFR 当拍点**：环回装置里，从站不会自己定时上送，需要"有人来问"才推进。
用 TESTFR 心跳当拍点既符合协议时序，又让 400 拍闭环能在测试里稳定重放。
现场自定时从站可 `set_poll_keepalive(false)` 关掉。

### 4.6 受控站仿真与注入点

`Iec104ControlledStationSim`：

- **出方向** `queue()` / `queue_i()` / `queue_measure_group()` / `queue_status_group()` / `queue_cyclic_burst()` ——
  空间不足**整帧丢弃**并计 `queue_overflows_`（不截断，避免发半个帧）；
- **入方向** `feed()` / `take()` / `process()` —— 攒帧解析，`handle_frame()` 分发；
- **诊断**：`vs()` / `vr()` / `gi_count()` / `setpoint_count()` / `bad_ca()` / `unknown_type()` /
  `malformed()` / `queue_overflows()`。

`LoopbackIec104Transport`：`inject_silence(n)` 让对端哑 N 拍（T45 用它触发 t3 超时 → FAULT）。

---

## 5. IDeviceIO 8 方法 ↔ 两种介质

| `IDeviceIO` 方法 | ModbusDeviceIO | Iec104DeviceIO |
| --- | --- | --- |
| `read_snapshot()` | 读块 0（`0x03`） | 取 `poll()` 落下的 ASDU 缓存 |
| `read_limits()` | 读块 2 + 块 3 | 取 CFG/STA 缓存 |
| `read_status()` | 读块 3 | 取 STA 缓存（遥信 + 遥测） |
| `write_command()` | 写块 1（`0x10`） | 3 条 `C_SE_NC_1` |
| `execute()` | 块写 / 单点写 | 同 `write_command()` 语义 |
| `data_valid()` | 在线 ∧ 品质 | 在线 ∧ 品质 ∧ 无致命遥信 |
| `self_check()` | 映射与点表逐点核对 | 同样逐点核对 |
| `name()` | 返回 "modbus" | 返回 "iec104" |

算法层（`EmsRuntime`）看到的调用序列与 `MemoryDeviceIO` 完全一致 —— 这就是"零改动"的含义。

---

## 6. 验证方法学

### 6.1 闭环逐位等价（核心证据）

```
prepare: 相同装配序列 configure_runtime(rt, io)   // 与 07/test_rtdb_device_io.cpp 逐行一致
         └─ 相同环境脚本 env_at(t)：负荷/光伏/电价/SOC 初值

run:     for t in [0, 400):
             rt.step(dt)          // 算法 11 步闭环，只经 IDeviceIO
             记录 StepRecord

compare: 基准 MemoryDeviceIO  vs  被测 ModbusDeviceIO / Iec104DeviceIO
         逐拍比对 p_cmd / p_actual / p_grid / soc / 区间 / 状态 / reason
```

T34 / T44 的实测结论：

- **宽精度通道**：400 拍 **diff = 0**（位级完全相同）；
- **现场标准通道**：**决策拓扑差异 = 0**，`|Δcmd|max = 3.93681e-05 kW`，`|ΔSOC| = 1.08913e-08`。

两条独立实现（Modbus 大端字序 vs IEC104 小端 ASDU）**推出同一个偏差上界**，互为旁证。

### 6.2 为什么宽精度通道能给出 diff=0

f64 在 Modbus 里占 4 个 16bit 寄存器、在 IEC104 里占 8 字节 ASDU —— 编解码是**纯位搬运**，
不经过任何有损转换。只要字序正确，`memcpy` 级别的恒等就成立。T31 / T42 分别断言了
"编码后再解码 == 原值（按位比较）"，这比"结果数值相近"强得多。

### 6.3 故障路径的验证

| 场景 | 构造方式 | 断言 |
| --- | --- | --- |
| 链路静默 | `set_link_up(false)` / `inject_silence(n)` | 状态机进 FAULT，`fault_bits` bit4（data_invalid） |
| 心跳丢 | 只丢 keepalive，遥测正常 | HOLD_LAST，指令冻结，权限区间钉成单点 |
| 设备故障 | 写 `STA.PCS_FAULT` 寄存器 / 发对应遥信 | FAULT → 门控归零 → 恢复后 READY 不自动带载 |
| 异常响应 | `inject_exception(0x02)` | 异常**不当数据**，缓存保留旧值 |

---

## 6.5 传输层：环回 → TCP（真内核 socket）

验证分三级，前两级都在"同调用栈内搬字节"，第三级才是真机形态：

```
第 1 级  编解码纯函数        modbus_codec.h / iec104_codec.h   逐字节断言（T31/T41/T42）
第 2 级  环回传输            Loopback*Transport                同栈搬字节，跳内核不跳字节
第 3 级  真内核 socket       tcp_transport.h                   独立线程 listen/accept + 127.0.0.1 回路
```

第三级换掉的只有 `IModbusTransport` / `IIec104Transport` 的实现，
适配器（`ModbusDeviceIO` / `Iec104DeviceIO`）与算法层（05/06/07/08）**一行不改**。

### 6.5.1 三个实现纪律

| # | 纪律 | 为什么 |
| --- | --- | --- |
| 1 | Modbus **按 MBAP `Length` 字段分帧**（先精确读 7 字节 MBAP，再按 `Length-1` 读 PDU），且 TID 回显必须一致 | TCP 是字节流：一次 `recv` 可能只回来半个响应（需重组），也可能把两个响应粘在一起（需切分）；TID 不一致 = 错位/串话，宁可直接丢弃也不能喂给解析器 |
| 2 | IEC104 的 `receive` **三态语义**：`false`=链路错误 / `true & len==0`=本轮无报文 / `true & len>0`=收到 N 字节 | 适配器 `drain_rx()` 靠这三态区分"对端哑了"（要计 link_error）和"这一轮就是没数据"（正常） |
| 3 | `connect` **非阻塞 + select 超时** | 阻塞 connect 连不通的地址会挂 20 s 以上，现场表现为"EMS 卡死"，比连不上更难排查 |

另有两条现场相关的细节：

- **`min_wait_ms`**：适配器的 `open()` / 总召唤用 `drain_rx(0)`（非阻塞）等确认帧 ——
  环回下响应同栈产生所以立刻可见，跨 TCP 必须等对端调度。给一个最小等待窗口，
  同一份适配器代码在两种介质下都成立；运行期回到 0（严格非阻塞）。
- **`set_expect_unit_id`**：Modbus 总线上有多个从站时，响应 UnitId 不一致必须拒收（防串话）。

### 6.5.2 跨线程独有的坑：滞后一拍（本节的真正价值）

104 与 Modbus 的语义差别在这里变成**测试装配问题**：

| | Modbus | IEC104 |
| --- | --- | --- |
| 取数方式 | 请求-响应（`transact`），天然同步 | 服务端**主动上送**，客户端只能"收" |
| 服务端位置 | 独立线程，但一问一答 | 独立线程，收到任一帧就按周期上送一轮 |
| 时序风险 | 无 | **上送时刻**与**客户端取值时刻**可能错位 |

具体表现是**逐拍比对整体滞后一拍、数值一个都不差**（不是偏差，是错位）。
根因有两条，缺一不可：

1. 服务端可能还在消化"上一拍发出去的帧"，它读到的设备值是上一拍的；这一轮上送的字节
   却在客户端**已经写入本拍环境之后**才被收走。
2. `EMS_P_GRID` 是 `MemoryDeviceIO::execute()` 里按 `P_LOAD + 站用电 − P_PV − P_BAT`
   现算的**派生点**（`SOC` / `T_C` / `P_BAT` 同理）；而 `realtime_loop.h` 第 ⑪ 步记录真值用的是
   `read_actuals()` —— 104 适配器读的是**最近一次上送的 cache**。
   环回装置下 `read_actuals()` 直接读设备，所以这个问题**在环回下根本不会出现**。

**处置（`tests/test_tcp.cpp`）**：不靠"调大超时"，而是**汇合 + 核对**。

```
Iec104TcpRig::sync_uplink(load, pv)          // 每拍开始，写在环境之前
  ① 读干 → 发一帧触发词 → 等服务端 round 自增 → 读干并丢弃（旧值上送全部作废）
  ② 写入本拍环境  ← 此后服务端的任何一轮上送都必然带本拍值
  ③ 触发 → 收干 → 核对 cache == 设备真值；对不上就再来（最多 4 轮）

Iec104TcpRig::refresh_after_execute()        // 设备被推进一拍之后（由 device_pump 触发）
  反复"触发 → 收干"，直到 cache 追上设备的 P_BAT / SOC
```

`round` 是服务端每消化完一批入向字节就自增的计数器，客户端用它确认"我发出去的帧已被消化完"
（TCP 是 FIFO，服务端既然处理到我的触发帧，之前的帧必然都处理完了）。
两个循环都带**失败计数**并在 T56 里断言为 0 —— 时序抖动允许"第二轮才对上"（`resyncs`），
但**不允许对不上**（`resync_failures` / `exec_refresh_failures`）。

据此 T56 在真 socket 上跑 400 拍得到 **逐位差异 0 拍**。

### 6.5.3 TCP 用例与验证点

| 用例 | 构造 | 断言要点 |
| --- | --- | --- |
| **T51** | 起 `TcpTestServer`（内核分配端口）；连接一个**已关闭的端口** 造连接失败 | 连接成功；失败在预算内返回（实测 510 ms < 2000 ms）；`receive` 三态 |
| **T52** | 服务端捕获每一帧 | 全帧 MBAP 自洽（PID=0 / UnitId=1 / 功能码合法）、出现过 `addr==0x0000` 的 MEAS 块读；SOC 与设备侧一致 |
| **T53(a)** | 服务端 `chunk=2`：每次只 `send` 2 字节 | 客户端自行重组，`stale_reads==0`（分片不能造成采集失败） |
| **T53(b)** | 服务端 `force_tid=0x0001`：响应事务号强制错位 | 请求#1（TID 一致）通过；请求#2 被拒（`format_errors==1` 且 `rn==0`）**但仍在线** |
| **T54** | 400 拍闭环 vs 进程内点表 | 逐位差异 0 拍；事务 2801；反向守卫：峰值 > 50 kW 且 SOC 首尾不同 |
| **T55** | IEC104 TCP 建链 | `startdt_ok` / `gi_done` / `unacked_tx==0` / 总召回数=1 / `bad_ca==0` / `malformed==0` |
| **T56** | 400 拍闭环（`kWidePrivate`） | 逐位差异 0 拍；`resync_failures==0`；`exec_refresh_failures==0` |
| **T57** | 服务端处理 800 帧后主动关闭 | 断链前峰值 100 kW；断链后 `stale_reads>0` 或 `link_errors>0` → FAULT → 指令归零、门控 |

**反向守卫**是这批用例的设计要点：等价性测试最容易被"两边都恒零"骗过，
所以 T54/T56 断言峰值指令 > 50 kW、SOC 首尾不同、T57 断言断链前确实在出力。

---

## 7. 点表契约（唯一的真相源）

```
07/src/rtdb/ems_point_table.h          ← C/C++ 共用的 30 点定义（唯一真相源）
        │
        ├──► ModbusRegisterMap  （P3）：按块展开 → 寄存器地址
        ├──► Iec104PointMap     （P3）：按段展开 → IOA
        ├──► MemoryDeviceIO.mem_point::  （P0.5）：点名常量
        └──► RtDbDeviceIO       （RT_DB）：共享内存点
```

四方**点名逐字相同**（`MEAS.SOC` / `CMD.P_BAT` / `CFG.SOC_MAX` / `STA.PCS_FAULT` ...）。
`self_check()` 在运行时逐点核对，`static_assert` 在编译期守点数 —— 双保险。

**扩容流程**：只改 `ems_point_table.h` → 重新编译时 `static_assert` 会报错提醒改映射 →
改完 `build_modbus_map()` / `build_iec104_map()` 两处 → 全部 T3x/T4x 契约用例自动覆盖新点。

---

## 8. 未覆盖项（明确边界）

- ~~**真实内核 socket**：环回刻意跳过；上真机只需补 `IModbusTransport` / `IIec104Transport` 的 TCP 实现。~~
  → **已在 §6.5 交付**：`tcp_transport.h`（`ModbusTcpTransport` / `Iec104TcpTransport`），
  Windows(Winsock2) / POSIX 双实现，T51~T57 真 socket 验证。**仍未做**：TLS / IEC 62351 认证、
  断线自动重连与主备双链路切换（当前断链只上报 FAULT，不自动重连）。
- **冗余双网 / SNTP 对时 / 文件传输（FTP 定值）**：调度接入的配套，未纳入。
- **Modbus RTU（串口）**：只实现 TCP；RTU 只是把 MBAP 换成 CRC 包裹，`modbus_codec.h` 已含 CRC16，
  补一个 Transport 即可，适配器不用改。
- **性能压测**：未做吞吐/时延基准；环回下 `transactions()` 可作观测点，但无硬性 SLA 断言。
