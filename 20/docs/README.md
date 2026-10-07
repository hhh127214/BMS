# 20/ — 告警能力与持久化

> **阶段成果。** 本模块清掉交接文档 `docs/交接/下一步工作交接说明.md` §3.6
> 点名「优先清掉」的三条技术债 —— 它们的共同点是
> **会让现场「出了问题看不见 / 恢复不了」**：
>
> | # | 债（原文） | 现场后果 | 本模块 |
> | --- | --- | --- | --- |
> | ①⑤ | **告警能力**（§2.2：`AlarmEntry`+`collect_alarms()` 寄生在仿真装配层） | 换个入口（`11/main_ems.cpp`、`14/` 后端）就**没有告警** | `src/alarm_model.h` + `src/alarm_assembler.h` + `src/alarm_input.h` |
> | ⑥ | **SOE 持久化**（`06/history()` 只在内存；P2 `SoeLog` 有界 4096 条） | 长跑丢历史；重启丢迁移链 | `src/soe_store.h` + `src/soe_dump.cpp` |
> | ④ | **配置持久化**（`04/` 只有 in-memory + `set_param`） | 现场调参一重启就没了 | `src/config_store.h` |

---

## 1. 它在整个工程里的位置

```
   10/sim_24h.h ──StepRecord──┐
                              ├─► 20/alarm_input.h ──► 20/alarm_assembler.h ──► 20/alarm_model.h
   P0 DeviceStatus/RealtimeSnapshot/DeviceLimits ─┘            │                       │
                                                               │                       └── 复用 P2 的
                                                               ▼                            SoeCode / SoeSource
                                        20/soe_store.h  ◄── AlarmEvent                    （不另造一套）
                                        20/config_store.h ◄── 04/StrategyManager::set_param 的 (id,key,value)

   复用（只读，不改）:  06/state_machine.h (EmsState)      05/safety_engine.h (SafetyParams)
                       04/data_models.h, 04/device_io.h    P2/soe.h (SoeCode/SoeSource/SoeLevel)
```

**边界（本模块不做什么）**：

| 不碰 | 为什么 |
| --- | --- |
| `10/src/sim_24h.h` 的 `AlarmEntry` / `collect_alarms()` | **不改旧文件**（硬约束）。本模块提供**替代路径**：`alarm_input_from_step_record()` 把同样的 `StepRecord` 喂进新装配器。旧函数原样留着，集成方决定何时把 10/ 切到新路径。 |
| `06/state_machine.h` 的 `StateEvent` | `StateEvent` 只有 `from/to/reason`，没有等级/事件码。**不加字段**（公共契约），等级由 `alarm_severity_for_state(to)` 在 20/ 侧派生。 |
| `P2/src/soe.h` 的 `SoeLog` | 它是**运行期有界缓冲**（如实上报 `dropped()`）；20/ 的 `SoeStore` 是**落盘不丢**。两者定位不同，不替换，只在其上加一层。 |
| `11/`、`14/` | 硬约束。本模块只提供适配器与接口签名；接线由集成方在 11/14 里做。 |

**依赖方向**：`20/ → {04,05,06,07(src),10,P2}`（只 `#include` 头文件，靠 `-I` 解析）。
没有任何旧模块依赖 20/。

---

## 2. 设计决策与「为什么」

### 2.1 告警等级：为什么不沿用 `SoeLevel`，也不自造

项目里已有**两套**等级口径，各有其位：

| 口径 | 取值 | 谁在用 |
| --- | --- | --- |
| `SoeLevel`（`P2/src/soe.h`） | DEBUG / INFO / WARN / ERROR / FATAL | 日志通道 |
| `alarm.level`（`14/schema.sql` 注释） | **INFO / WARNING / DERATED / FAULT / EMERGENCY** | 平台库（告警表） |

`AlarmSeverity` **逐字采用 `14/schema.sql` 的取值**（平台库口径），
并用显式映射 `soe_level_of()` 连到日志通道 —— 不隐式转换。
理由：告警要写进平台 `alarm` 表、要按严重度过滤与统计，
口径必须和库 schema 一致；而 `DERATED`（可自恢复的降功率）
在日志口径里没有对应档，折到 `WARN`，**但条目自身的 severity 不降级**。

> 决策依据：约定 §7-3「改公共契约用只读视图，不要改公共结构体」。
> 加一个 `AlarmSeverity` 是**新增**，不是改 `SoeLevel`。

### 2.2 事件码 / 来源：**复用 P2，不另造**

`alarm_model.h` 里 `using AlarmCode = SoeCode; using AlarmSource = SoeSource;`
—— 事件码表已经在 `P2/src/soe.h` 存在（40 个码，含 Start/End 配对、
通信类、设备类、协同类、系统类），再造一套只会产生"同一事实两处不同步"。

SET↔CLEAR 的配对关系用一张**只读映射表** `alarm_clear_code()` 表达，
而不给 `SoeCode` 加字段（那会波及 P2/P3/11/14）。

### 2.3 处置动作：为什么是「机器可读的表」而不是注释

`11/docs/README.md` §4 的「六类故障源 × 谁该动」是**联调固化下来的**，
不是设计推演。本模块把它编译成 `alarm_policy(code) → {severity, action, owner,
device_self_protect}`，**逐条可断言**（见 §4 的对齐结果）。

为什么必须这样：如果处置只写在文档里，现场新入口就可能"报了告警但没人动"。
把「谁该动」变成返回值，`test_alarm_model.cpp` 的 T02 才能在编译期/测试期钉住它。

**关键的语义分界（§4 表原文）**：

- `kind 2 电表通信丢失` **不进 FAULT** —— 超阈值走 `HOLD_LAST`（接口规范 §6）。
  所以它的 severity 是 `WARNING`，不是 `FAULT`。把电表丢帧判成 FAULT 会让
  EMS 一抖动就丢控制权。
- `kind 6 数据品质位劣化` **进 FAULT** —— 「值仍是值，只是不可信；
  设备照常执行指令」，所以设备侧 fail-safe = 否。
- 只有 `kind ∈ {3,4,5}`（PCS 通信丢失 / PCS 故障 / 设备离线）
  要求**设备侧** fail-safe。

### 2.4 输入为什么是 `AlarmInput`，不是 `Sim24hConfig`

`AlarmInput` 只有：当前时间、状态（含迁移）、四个物理量、六个设备状态位、
两个运行期标志（`data_stale` / `safety_clip`）—— **没有一处指向仿真**。

两个适配器覆盖项目仅有的两类入口：

| 适配器 | 数据来源 | 对应现场 |
| --- | --- | --- |
| `alarm_input_from_step_record()` | `10/` 的 `StepRecord` | 过渡期回归 |
| `alarm_input_from_production()` | `RealtimeSnapshot` + `DeviceStatus` + `DeviceLimits` + `EmsState` | `11/main_ems.cpp`、`14/` 后端 |

**两个适配器共用一套故障口径**（`fault_view_from_status()` 与
`fault_view_from_bits()` 都对齐 `EmsRuntime::detect_faults()`）。
不共用的话，"设备离线"在两条路径上会派生出不同的告警（仿真侧
`FaultFlags::bits()` 里 `bms_comm_lost = !bms_comm_ok || device_offline`，
生产侧 `DeviceStatus::bms_comm_ok` 仍是 true）—— 等价性测试就成了假的。
这条在 T10 有专门断言。

### 2.5 边沿语义：为什么"持续期间只出一条"

`10/` 的 `collect_alarms()` 手工实现了"记首次 + 恢复"，但**每个指标都要手写一遍**，
漏一个就退化成事件风暴。P2 的实测：收紧配置下 `safety_clip` **连续 838 拍**。

本模块把边沿检测做成**框架能力**：条件成立只出 1 条 SET，`repeat` 记持续拍数，
解除时出 1 条 CLEAR。阈值类另加**回差**，避免在边界抖动时反复 SET/CLEAR。

判据有效性（见 §3 坑 3）：`Config::edge_detection` 可关，关掉后同一场景
产生 `N` 条而不是 1 条 —— 若有人把边沿检测改坏，`size()==1` 那条断言立刻红。

### 2.6 SOE：为什么"文件是唯一真相源"

P2 的 `SoeLog` 有界 4096 条，超出**淘汰最旧**并计 `dropped()`。
如实上报丢失是对的，但现场要的是**不丢**。

`SoeStore` 的做法：

- `append()` **先写盘 + flush，再入内存**。返回 `true` ⇒ 该条已持久化。
- 内存分两块：**索引**（全量，`{seq, offset, t, level, source, code}`）
  与**热缓存**（有界 `memory_capacity`）。超容量淘汰的是**热缓存**，
  索引与文件都在 → 检索仍返回全部。这就是"超出容量上限时不丢"。
- 文件是 `append-only` 的**结构化文本**（一行一事件，见 §2.7），
  运维可直接 `grep` / `wc -l` / 用 Python `csv` 读。

### 2.7 文件格式：为什么让"一条事件必须一行"

```
# EMS_SOE v1  fields: seq,t,level,source,code,kind,value,message
3,101.000000,ERROR,FSM,FSM_TRANSITION,SET,0,"state NORMAL -> FAULT"
4,101.000000,ERROR,COMM,COMM_LOST_BMS,SET,0,"bms comm lost"
7,201.000000,ERROR,COMM,COMM_RESTORED,CLEAR,0,"cleared: COMM_LOST_BMS"
```

- `message` 里的真实换行**转义**成字面 `\n`。若允许引号内嵌真换行，
  一条事件会跨多行，运维 `wc -l` / `grep` 统计全线错位（P2 已记录）；
- `\` → `\\`、`"` → `""`，保证任意消息都能**无损往返**；
- 第 1 行版本头 —— 外部工具据此识别格式。

**`--dump` 例子**（`20/src/soe_dump.cpp`，编译进 `build/soe_dump.exe`）：

```bat
20\scripts\build.bat
20\build\alarm_demo.exe
20\build\soe_dump.exe --file 20\build\soe.log --min-level WARN
20\build\soe_dump.exe --file 20\build\soe.log --source COMM --t0 100 --t1 500 --out 20\build\soe.comm.log
```

实测输出（`--min-level WARN`，INFO 条目被过滤，故 seq 有跳号）：

```
# EMS_SOE v1  fields: seq,t,level,source,code,kind,value,message
3,101.000000,ERROR,FSM,FSM_TRANSITION,SET,0,"state NORMAL -> FAULT"
4,101.000000,ERROR,COMM,COMM_LOST_BMS,SET,0,"bms comm lost"
5,101.000000,WARN,SAFETY,SAFETY_CLIP_START,SET,0,"safety clip active"
7,201.000000,ERROR,COMM,COMM_RESTORED,CLEAR,0,"cleared: COMM_LOST_BMS"
...
file=build\soe.log total=16 matched=12 min_level=WARN
```

### 2.8 配置：为什么缺字段必须"指名报错"，不允许默认值

`11/docs/README.md` §5.5（缺口 A2）记着一次教训：改对了实现，却在所有既有夹具下
**逐位恒等** —— 因为下游"静默降级"掩盖了差异。配置缺字段若静默取默认值，
现场表现就是「EMS 用错误参数在跑」，且日志里没有任何异常。

所以 `ConfigStore` **没有** `get_or_default()`：

- `get(group, name, &out)` 字段不存在 → 返回 `false`；
- `load()` 缺必填字段 → `ok=false`，`missing_fields` 列出**每一个**缺的字段，
  `error = "missing required field: <group>.<name>"`；
- **原子性**：只要有缺字段，文件里的其它字段也**不加载**（避免"部分加载"这种
  半成品状态）。T04 有断言。

文件格式（同样稳定、可外部读）：

```
# EMS_CONFIG v1
VERSION|1
FIELD|S07_PEAK_VALLEY|P_discharge|80
FIELD|S04_DEMAND_MGMT|d_target_kw|400
CHANGE|100.000000|operator|S07_PEAK_VALLEY.P_discharge|0|80|1
CHANGE|150.000000|engineer|S07_PEAK_VALLEY.P_discharge|80|60|0
```

`group` 用策略 id、`name` 用参数名 —— 与 `04/StrategyManager::set_param(id, key, value)`
一一对应，因此 `EmsRuntime` 里的真实参数可以直接导出/回灌（T07 用真 runtime 验证）。

---

## 3. 踩坑记录（现象 / 根因 / 修法 / 判据有效性）

### 坑 1：`SoeStore::open()` 新建文件不写版本头 → 格式断言全红

- **现象**：`test_soe_store` S06 三条断言红：`first.rfind("# EMS_SOE v1",0)==0` 失败、
  文件物理行数 `2 != 3`、样例行的前缀匹配失败。
- **根因**：`open()` 只在"扫描已有文件"时**读**头行，却从不在**新建**时**写**头行。
  于是全新文件的第一行就是第一条事件，版本契约当场不成立。
  这是"写路径"与"读路径"不对称导致的 —— 读路径认识头行，写路径不生产头行。
- **修法**：扫描时记录 `any_line`；若文件为空，在打开追加流后先写
  `header_line()` 并推进 `write_off_`。
- **判据有效性**：S06 直接断言头行前缀 + 物理行数。退回旧实现 → 立刻红。

### 坑 2：`AlarmAssembler(ctx, Config{...})` 在 g++ 8.1 上编译不过

- **现象**：`error: default member initializer for 'Config::edge_detection'
  required before the end of its enclosing class`。
- **根因**：把 `const Config& cfg = Config()` 写成**同一类内嵌套类型**的默认实参，
  GCC 8.1 认为该类型的默认成员初始化器在类内尚未完成。（GCC 9+ 放宽了。）
- **修法**：拆成两个构造函数（`AlarmAssembler(ctx)` 与 `AlarmAssembler(ctx, cfg)`），
  不用默认实参。项目要求 g++ 8.1，这类写法必须避开。
- **判据有效性**：编译期错误，无法静默。

### 坑 3：反向守卫必须"造出可区分差异"，否则断言测的是别的东西

- **现象**：T04 断言"持续 100 拍只产生 1 条 SET" —— 这条断言**只在边沿检测正确时**
  才有意义。如果实现退化成"逐拍记录"，它同样会红（好），但如果实现退化成
  "什么都不记"，`size()==1` 也会红但红得**没有信息**，无法区分"去抖正确"
  与"根本没触发"。
- **根因**：本项目约定 §3.1 的 A2 教训 ——「先造出能区分对错的可观测差异」。
- **修法**：加 `Config::edge_detection`（默认 `true`）这个**可关闭的开关**，
  T05 用同一场景跑两个模式：开 → 1 条、关 → 100 条。于是 `size()==100`
  是"真的测到了逐拍记录"，`size()==1` 才是"边沿检测生效"。
- **判据有效性**：把 `edge_detection` 的默认值改成 `false` → T04 立刻红
  （实测：`size()=1 vs 100`）。
- **同类**：T11 的等价性断言配了第二条守卫 —— 扰动路径 B 的一个快照字段
  （多置一个 `pcs_fault`）后必须**不一致**，证明"一致"不是恒真。

### 坑 4：`StepRecord::reason` 不是状态机的迁移理由

- **现象**：第一版 `alarm_input_from_step_record()` 把 `rec.reason` 当迁移原因，
  结果迁移事件的 message 变成 `(+state_gate:INIT)` / `(+l2_correction)` 这种
  **指令理由**，语义错位。
- **根因**：`EmsRuntime::step()` 里 `rec.reason = cmd.reason;` ——
  `StepRecord.reason` 记的是**指令**的理由；状态机迁移理由在
  `fsm().last_reason()` / `fsm().history()` 里。
- **修法**：仿真入口不再取 `rec.reason`，迁移原因留空（生产入口可传
  `transition_reason`）。已列为已知边界。
- **判据有效性**：T10 的迁移事件 message 由生产入口显式提供
  `transition_reason`，与仿真入口的"空"形成对照；两者事件流仍一致（因为
  比对的是 code/severity/t，不含 message）—— 这也说明该字段是**展示信息**，
  不参与判定。

### 坑 5：`.bat` 里"中文紧邻变量"与"中文后接数字"两条静默坑（预防，未复发）

- **现象**（本项目其它模块实测，见约定 §4）：`.bat` 是 UTF-8、cmd 按 CP936 解码，
  某些中文字节与紧随的 `!`(`0x21`) 配对被吃掉；中文 `echo` 行后的数字也会被吞。
  退出码仍是 0，只有那一行坏。
- **修法**（本模块三处都遵守）：
  1. 含 `!VAR!` 的行**全部纯 ASCII**（`g++ ... !INC! ...`、`set /a TOTAL_PASS=!PASS_A!+...`）；
  2. 汇总数字**单独占一行且纯 ASCII**（`TOTAL_ASSERTIONS=397` 独占一行）；
  3. 多语句分支用 `goto` + 标签，不用 `if (...)` 块；
  4. 汇总横幅（含中文）**不带变量**，且**后面不接数字**；
  5. 汇总数字**不从中文 stdout 里 grep**，而是读每个 exe 落的
     纯 ASCII 状态文件 `build/test_*_status.txt`（沿用 `13/` 的 `bridge_status.txt` 先例）。
- **判据有效性**：`python scripts/fix_bat_encoding.py --check 20/scripts` 返回 0；
  `build_test.bat` 实测 `TOTAL_ASSERTIONS=397 / TOTAL_FAIL=0`，数字完整。
  （旁证：同一脚本里一句**纯展示用**的中文横幅 `=== 20/ 告警能力与持久化 测试汇总 ===`
  在 CP936 控制台下确实出现了末尾字符被吞的显示乱码 —— 正因如此，
  所有**参与判定**的行才必须是纯 ASCII。）

### 坑 6：回差滞环"查的键"和"写的键"不一致 → 滞环静默失效

- **现象**：`AlarmAssembler` 的 SOC 回差看起来实现了，测试也**全绿** ——
  但把 SOC 从 0.09 抬到 0.105（仍落在 0.10~0.12 的回差带内）时，
  告警**当场就 CLEAR 了**，回差等于不存在。
- **根因**：`latched(key, ...)` 用来判断"上一拍是否已处于告警态"的键写成了
  手写字面量 `"SOC:HIGH"`，而 `active_` 这个 map 的键是 `Cond::key()` 生成的
  `"SAFETY:SOC:SOC_HIGH_START"`。两个字符串不一致 → `active_.find(k)`
  **永远返回 `end()`** → 滞环分支永不进入（四个阈值类条件全中，不止 SOC）。
- **为什么"测试全绿"**：当时的 T06 只断言 SET/CLEAR 的**条数**（2/2），
  而"提前 CLEAR"同样给出 2 条 —— 断言**没有区分度**，正好又撞上约定 §3.1 的
  A2 教训。这是本轮唯一一个"实现错、而测试也测不出"的缺陷。
- **修法**：抽出 `cond_key(source, source_id, code)` 作为**唯一**的键构造函数，
  `Cond::key()` 与 `latched()` 都调它；并把 T06 的判据从"数条数"改成
  **断言 CLEAR 的时刻**（低限必须 `t=5.0`、上限必须 `t=9.0`）+ `repeat` 覆盖回差带。
- **判据有效性**：去掉回差（`latched` 只用裸阈值 `bad`）→ CLEAR 提前到
  `t=3.0` / `t=8.0` → T06 两条断言立刻红。**这次才真正测到了回差。**

---

## 4. 测试清单

三个可执行文件，各自一个 `main`，末尾印 `PASS=%d FAIL=%d`，
并落一份纯 ASCII 状态文件供 `build_test.bat` 汇总。

| 可执行文件 | 断言 | 用例 |
| --- | --- | --- |
| `test_alarm_model.exe` | **176** | T01~T12 |
| `test_soe_store.exe` | **144** | S01~S08 |
| `test_config_store.exe` | **77** | C01~C09 |
| 合计 | **397** | FAIL=0 / SKIPPED=0 |

### 4.1 `test_alarm_model.cpp`

| 用例 | 覆盖 | 关键断言 |
| --- | --- | --- |
| T01 | 等级口径 | 五个名称/顺序与 `14/schema.sql` 一致；名称往返；`soe_level_of()` 映射；`WARN`（旧 10/ 口径）被拒绝 |
| T02 | **六类故障源 × 谁该动**（11/§4） | 逐条断言 severity/action/owner/`device_self_protect`；反向守卫：`device_self_protect==true` 恰好 3 条、`GATE_ZERO` 恰好 5 条 |
| T03 | SET/CLEAR 成对 | 2 条事件、`pair_id` 相同、`duration=10`、`repeat=10`、`key()` 格式 |
| T04 | 不重复刷屏 | 持续 100 拍 → `records()==1`、`repeat==100` |
| T05 | **反向守卫：去掉边沿检测** | 同场景 开=1 条 / 关=100 条；SOC 持续越限 50 拍 → 1 条 |
| T06 | SOC 越限 + **回差** | set=2/clear=2；**★ 解除时刻**：低限必须 `t=5.0`、上限必须 `t=9.0`（去掉回差 → 提前到 3.0/8.0 → 红）；`repeat` 覆盖回差带（4 / 3） |
| T07 | 温度 | 预警 1 条；≥`temp_fault_c` → `EMERGENCY` 1 条 |
| T08 | 变压器/需量/防逆流 | 三类各 1 条；反向守卫：关掉 `forbid_reverse` → 倒送告警 0 条 |
| T09 | 状态迁移点事件 | 7 次迁移 → 7 条（**不被合并**）；FAULT/EMERGENCY 各 1；`is_point()` |
| T10 | **生产入口（零仿真结构体）** | 只用 P0 结构体驱动；断言 BMS 通信 2 次（含"离线派生"）、设备离线 1 次、迁移 4 次；逐条核对处置 |
| T11 | **仿真入口 vs 生产入口 等价** | 同一装配器，两条输入路径事件流 `same_stream()` 逐条一致；反向守卫：扰动一条快照 → 必须不一致 |
| T12 | 顺序稳定 | 同拍多条件都报；第二拍 0 事件；两次独立跑序列一致 |

### 4.2 `test_soe_store.cpp`

| 用例 | 覆盖 | 关键断言 |
| --- | --- | --- |
| S01 | 追加写 / 读回 | 20 条、seq 1..20、逐条时间与消息 |
| S02 | **重启后逐条一致** | 写 64 条 → 析构 → 重新 open → `identical==64`（含首/末） |
| S03 | seq 连续 | 重开后 `last_seq==64`，再 append → 65 |
| S04 | **超容量不丢** | `memory_capacity=8`、写 500：`memory_size==8`、`dropped_from_memory==492`（反向守卫）、**`size==500`**、逐条 seq/时间都在、首条仍可读回 |
| S05 | 检索 | 时间范围 10 条、等级 0/500、来源 500/0、组合 100 条 |
| S06 | 文件格式稳定 | 头行前缀；含 `,` `"` `\` 换行的消息仍是**一条一行**（物理行数 3）；`parse_file` 独立解析成功且消息往返一致；样例行前缀与文档逐字一致 |
| S07 | 与告警装配器桥接 | `AlarmEvent` → SOE：SET/CLEAR 各 1 条；FAULT→`ERROR`；`COMM_RESTORED` |
| S08 | 损坏行显式报错 | `open()` 失败且 `error` 含 `corrupt SOE line` 与 `offset`（不静默跳过） |

### 4.3 `test_config_store.cpp`

| 用例 | 覆盖 | 关键断言 |
| --- | --- | --- |
| C01 | 字段级往返 | 3 字段 set→save→load→逐字段一致（`EXPECT_NEAR`，不用整型截断） |
| C02 | 重启后值不丢 | 改一个值再存，再重启：新值在、未动字段也在 |
| C03 | 变更台账 | 3 次变更（值未变不记）；`field/old/new/t/who/created` 逐项；台账也落盘回读 |
| C04 | **缺字段显式报错** | `missing_fields==["S04_DEMAND_MGMT.d_target_kw"]`；`error` 指名；**反向守卫**：`get()` 返回 false 且 out 未被改动；**原子性**：整体不加载 |
| C05 | 损坏行 | `corrupt_line==4`、`error` 含 `line 4`；未知记录类型也被报出 |
| C06 | 版本不匹配 | `error` 含 `version mismatch` 与 `99` |
| C07 | **对接 04/ 真实参数** | 用真 `EmsRuntime`：调参 → 导出 → 落盘 → 重启 → 回读逐字段一致 → 回灌到新 runtime，`P_discharge==88`、`d_target_kw==450` |
| C08 | 未知字段 | `unknown_fields==["X.Y"]`；未知字段**保留**不静默丢 |
| C09 | 无默认值语义 | 不存在字段 `get()==false`；没有 `get_or_default` 这类接口 |

### 4.4 六类故障源对齐结果（T02，逐条）

| kind | 故障源 | `alarm_policy` severity | action | owner | `device_self_protect` | 与 11/§4 一致 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | BMS 通信丢失 (bit0) | `FAULT` | `GATE_ZERO` | EMS | false | ✔ |
| 2 | 电表通信丢失 (bit2) | `WARNING`（**不进 FAULT**） | `HOLD_LAST` | EMS | false | ✔ |
| 3 | PCS 通信丢失 (bit1) | `FAULT` | `GATE_ZERO` | BOTH | **true** | ✔ |
| 4 | PCS 故障 (bit3) | `FAULT` | `GATE_ZERO` | BOTH | **true** | ✔ |
| 5 | 设备离线 (bit5) | `FAULT` | `GATE_ZERO` | BOTH | **true** | ✔ |
| 6 | 数据品质劣化 (bit4) | `FAULT` | `GATE_ZERO` | EMS | false | ✔ |

> §4 判据原文：`dev_self_protect = (kind ∈ {3,4,5})` → 本表 true 的恰好是 3 条。

---

## 5. 容量算式（SOE 落盘）

**单条事件行开销**：固定字段（`seq`/`t`/`level`/`source`/`code`/`kind`/`value`
+ 引号与逗号）实测约 **60~70 B**，加 `message`（典型 10~40 字符）后约
**70~110 B**。取 **S ≈ 100 B/条** 估算。

```
磁盘占用  = 保留天数 T × 每天条数 D × S
保留天数  = 磁盘配额 Q / (D × S)
索引内存  ≈ 条数 × 48 B        （全量索引，见 §6 已知边界）
热缓存内存 ≈ memory_capacity × ~150 B   （默认 4096 → ~0.6 MB）
```

**正常日**（P2 口径：抑制后 ≤ 200 条/天，本项目实测 24h 仿真 `SOE < 200`）：

| D | T | 磁盘 | 索引 |
| --- | --- | --- | --- |
| 200 条/天 | 365 天 | **7.3 MB** | 3.5 MB |
| 200 条/天 | 3650 天 | 73 MB | 35 MB |

**故障/风暴日**（抑制后仍有 ~5000 条/天，含 SET/CLEAR 对）：

| D | T | 磁盘 | 索引 |
| --- | --- | --- | --- |
| 5000 条/天 | 365 天 | **183 MB** | 88 MB |
| 5000 条/天 | 30 天 | 15 MB | 7.2 MB |

**"什么落盘周期不会丢"**：

- 默认 `flush_each=true`：**落盘周期 = 事件到达周期**。`append()` 返回 `true`
  即已 `flush` 到文件；`false` 表示未持久化（未 `open` / 磁盘满），
  **调用方必须当作"没记上"**（返回值语义在头文件里写明）。
- 若设 `flush_each=false`：未落盘窗口 ≈ `flush 周期 × 事件速率 R`
  （`R` 条/s → 掉电丢 ≤ `R × flush周期` 条）。此时"不丢"要求把 flush 周期
  压到可容忍的丢失窗口内。**本模块测试用的是默认 `true`。**
- 一句话容量判据：**要保留 T 天、事件率 R 条/s，需磁盘 ≥ `T·R·86400·100 B`,
  且内存 ≥ `T·R·86400·48 B`（索引）；二者任一不足就要做轮转**。

---

## 6. 已知边界（诚实列出，未做的**不**写"已完成"）

1. **SOE 索引是全内存的。** `index_` 随文件无上限增长（≈48 B/条）——
   超容量淘汰只作用于**热缓存**，索引不淘汰。按 §5，索引在 1.8 M 条/年时约 88 MB。
   **未做**：分段文件 + 每段独立索引、时间桶索引、压缩。现场长期运行必须配轮转
   （定期切文件、归档旧段）。
2. **`SoeStore` 是单写者假设。** 未做跨线程/跨进程写保护；也未做文件锁。
   多进程同时 append 会破坏 seq 单调与偏移索引。
3. **SOE 时间戳精度 `%.6f`。** 若直接用 Unix 纪元秒（~1.7e9），double 的
   15~16 位有效数字只剩 ~1e-6 的绝对分辨率，边界处可能丢末位。
   测试用仿真秒（0~600），故"逐条一致"成立。建议现场存**相对基准时刻**的秒数，
   或把格式降到 `%.3f`（毫秒）。
4. **告警的"需量超契约"是瞬时判据**（`p_grid > d_target`），
   与 `08/` 的需量**窗口均值**口径不同。这是简化，现场可能出现
   "瞬时越限报一条、但窗口均值并未越契约"。**未做**窗口化。
5. **仿真入口拿不到状态机迁移理由** —— `StepRecord` 不带 `fsm().last_reason()`
   （坑 4）。生产入口可传 `transition_reason`，仿真入口留空。
6. **等价性测试是"公共产物 → P0 契约"的改写**，不是两条**完全独立**的数据链。
   它证明的是：装配器对同一份信息、两种表示给出同一结果，且扰动有区分度。
   **未做**：在 `11/` 的真三进程链路里跑同一装配器（硬约束不允许改 11/）。
7. **生产入口的 `data_stale` 需要调用方显式给。** `EmsRuntime` 内部算了
   `hold_last`，但没有对外 getter（`StepRecord::hold_last` 只在日志里）。
   `ProductionSnapshot::data_stale` 因此是显式入参。
8. **告警的确认（ACK）/ 抑制窗口 / 通知外送未做。** 本模块只做"识别 + 记录 + 落盘"。
   现场需要的"抑制配置下发""告警确认状态机""短信/邮件外送"都不在这里。
9. **配置持久化只覆盖 `double` 型参数**（与 `04/ParamMap` 一致）。
   字符串 / 布尔 / 枚举型配置不支持。
10. **`ConfigStore` 的 `group`/`name`/`who` 不允许含 `|`**（解析分隔符）。
    已在校验里显式报错，不静默。
11. **`alarm_demo.exe` 用的是仿真入口**（`10/`）。生产入口的可运行演示
    需要 `11/14` 的接线，由集成方完成。
12. **脚本未接入 `scripts/build_all.bat`。** 按约定 §6，最后一步由**集成方**
    统一执行（避免并发改同一文件）；本模块只保证 `build_test.bat` 退出码 0、
    `FAIL=0`、打印断言总数。

---

## 7. 目录结构、依赖方向、编译参数

```
20/
├── src/
│   ├── alarm_model.h       AlarmSeverity / AlarmRecord / AlarmEvent / alarm_policy()
│   ├── alarm_assembler.h   AlarmContext / AlarmInput / AlarmAssembler
│   ├── alarm_input.h       两个入口适配器 + ProductionSnapshot + AlarmContext 构造
│   ├── soe_store.h         SoeStore / SoeStoreRecord（落盘 + 索引 + 检索）
│   ├── config_store.h      ConfigStore / ConfigField / ConfigChange
│   ├── soe_dump.cpp        --dump 工具（读/过滤/导出 SOE 文件）
│   └── alarm_demo.cpp      端到端演示（仿真 → 告警 → 落盘）
├── tests/
│   ├── test_alarm_model.cpp      T01~T12（176 断言）
│   ├── test_soe_store.cpp        S01~S08（144 断言）
│   └── test_config_store.cpp     C01~C09（ 77 断言）
├── scripts/
│   ├── build.bat           编译 soe_dump.exe / alarm_demo.exe
│   └── build_test.bat      编译 + 运行三个测试，汇总断言总数
├── docs/
│   └── README.md           本文件
└── build/                  产物（.exe / .log / .ini / 状态文件），不入库
```

**依赖方向**：`20/ → {04, 05, 06, 07(src), 10, P2}`（只 `#include` 头文件）。

**编译参数**（`build.bat` / `build_test.bat` 一致）：

```
g++ -std=c++17 -Wall -O2 ^
    -I src -I ..\04\src -I ..\05\src -I ..\06\src -I ..\07\src -I ..\08\src ^
    -I ..\07\src\rtdb -I ..\10\src -I ..\P2\src
```

工具链：MinGW-w64 **g++ 8.1.0**，C++17。**避开 `<filesystem>` 与 `<charconv>`**
（g++ 8.1 上不完整 / 需额外链接），本模块只用 `<fstream>` + `<cstdio>` + `<string>` + 手写解析。

**运行**：

```bat
20\scripts\build_test.bat        :: 期望末行 [OK] 20\ tests all passed，TOTAL_ASSERTIONS=397
20\scripts\build.bat             :: 编译 soe_dump.exe / alarm_demo.exe
20\build\alarm_demo.exe          :: 端到端演示
20\build\soe_dump.exe --file 20\build\soe.log --min-level WARN
```

> 注：`.bat` **不能从 Git Bash 直接跑**（仓库路径含括号 `(cn)`，见约定 §4.1）。
> 用 Python 起子进程：
> ```python
> subprocess.run(['cmd','/c','build_test.bat'], cwd=r'D:\wb(cn)\BMS\20\scripts')
> ```
