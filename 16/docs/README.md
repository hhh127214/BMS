# 16/ — 通信加固（退避重连 / 双链路 / 对时 / RTU / FTP 定值 / TLS）

> 本模块的定位是**上现场前必须补的工程欠项**：前面 13/ 把"协议能通"做完了，
> 但现场真正会把人困住的是另一批问题 —— 设备反复掉线、只有一条链路、
> 没有可信时钟、串口设备接不进来、定值下发的通道不透明、以及"我们的数据要不要加密"。
>
> 本模块**只做这些**。它不是新业务功能。
>
> 措辞约定：本文一律写「已完成 / 阶段成果」，不写「交付 / 交付物」——
> 本工程尚未全部完成（见 §7 已知边界）。

---

## 1. 它在整个工程里的位置

```
                  ┌──────────────────────────────────────────┐
                  │  16/ 通信加固（本模块，header-only）       │
                  │  退避 · 双链路 · 对时 · RTU · FTP · TLS    │
                  └──────────────────────────────────────────┘
                        ▲                     ▲
        （被上层调用）    │                     │  （自带独立测试）
                        │                     │
   ┌────────────────────┴───┐        ┌────────┴──────────────────┐
   │ 13/ Modbus TCP 设备接入 │        │ 16/tests/ 五层测试         │
   │ 14/…/15/ 其它设备接入    │        │ 自起进程内服务端，不依赖公网 │
   └────────────────────────┘        └───────────────────────────┘
```

依赖方向：**16/ → {自身、Windows 系统库}**。16/ **不**依赖 13/～15/，
也**不**被 13/～15/ 依赖（见 §6.4 关于 13/ 集成点的说明 —— 这一步目前**没做**）。

16/ 与相邻模块的边界：

| 相邻 | 边界 |
| --- | --- |
| 13/ Modbus TCP | 13/ 管"点表映射 + 快照语义 + 指令原子性"；16/ 只提供**可插拔的重连退避策略**与**双链路仲裁**，不理解任何点表 |
| 17/ 18/ 19/ | 并列的加固模块，互不依赖。16/ 的 `ITransport` 与 17/ 若要做通道复用，接口应复用本文件的抽象而不是另起一套 |
| `P1/P2/P3` | 无关。P3 的 `vendor/lib60870` 是全工程唯一的第三方依赖例外；16/ **零第三方依赖** |
| 现场设备 | 16/ 是**客户端**（PCS / BMS / 电表的应答方视角），不做任何服务端实现 |

---

## 2. 设计决策与"为什么"

六个头文件，每个都回答一个"不这么做会怎样"。

### 2.1 `src/backoff.h` — 重连退避

**做了什么**：`BackoffPolicy{initial_ms=1000, max_ms=30000, factor=2.0, jitter_ratio=0.2}`
+ `class Backoff`（`next_delay_ms()` / `peek_next_delay_ms()` / `reset()` / `attempt()` /
`total_wait_ms()`），PRNG 是**确定性** xorshift64\*。

**为什么必须确定性**：随机抖动写成 `rand()` 的话，"同一种子同一序列"这条判据
根本立不起来 —— 测试只能在统计意义上断言，出了偏差无法复现。
确定性 PRNG 让抖动**既随机又可复现**，于是可以硬断言
"同一 seed 必须逐位同序列"（T03）与"抖动倍数落在 [1-r, 1+r]"（T02）。

**为什么必须有 `factor=1.0 & jitter_ratio=0` 这条路径**：它是与 13/ 现行为
接轨的**接缝**。13/ 现在的重连是固定 1000 ms；只有保证
"`Backoff` 在 factor=1、jitter=0 时逐次恒等于 `initial_ms`"（T05 硬断言），
替换策略才敢说是"行为等价的可插拔升级"，而不是"顺手改了现场节奏"。

**非法 policy 必须显式归一而不是静默正确**：`factor<1` 会产生**递减**序列，
`initial>max` 会让封顶逻辑永远不可达。两者的共同后果是延迟算出 0 或负数 ——
一个"立刻无限重连"的进程会把 200 ms 的控制周期打穿。
所以 `validate()` 给出中文原因、`normalized()` 给出可用值、`normalize_reason()` 说明改了什么，
且 T06 钉住"非法必被归一"，T09 钉住"合法**不**被归一"（反向守卫：
没有 T09，把所有输入都归一也照样能过 T06）。

### 2.2 `src/link_arbiter.h` — 主备双链路仲裁

**做了什么**：`LinkArbiter<PrimaryLink, BackupLink>`，主链路连续 N 次失败切备、
主链路连续 M 次成功切回；`current()` / `switches()` / `failovers()` / `failbacks()`。

**为什么用"契约检测"而不是 C++20 concepts**：工具链是 g++ 8.1 / C++17。
`is_link_contract<L>`（SFINAE 探测 `connect/close/is_up/endpoint`）在编译期检查
链路类型是否合格，写错接口时是**编译错误**而不是运行期崩溃（T19 钉住检测器本身有区分度：
故意给一个缺 `connect()` 的 `BadLink`，检测器必须说 false）。

**为什么 M 必须大于 N**：M ≤ N 时"失败 N 次切备、主链成功 M 次切回"会在
阈值附近形成**切换风暴** —— 主链半死不活时每次探测都来回切，
对端会不断看到新会话、SOE 里会出现成串的通道切换记录。
`normalized()` 强制 `M = N+1`（T17），T14 则用"固定驱动序列 + 切换次数上界"证明
滞环真的把风暴压住了，**并且**用一个"无滞环的对照实现"跑同一序列拿到 ≥10 次切换，
证明这条上界不是"条件没被触发"（判据有效性反证）。

**为什么"两条都断"要单独测**（T15）：最容易犯的错是"备链也断时继续切"，
形成 `is_up()==false` 却每拍切换一次的忙循环。T15 同时钉住
`is_up()==false` 与切换次数不增长。

### 2.3 `src/sntp.h` — SNTP 对时

**做了什么**：RFC 4330 单播客户端（48 字节报文、LI/VN/Mode、stratum、
originate/receive/transmit 三个时间戳）+ `ClockSync`（单调钟 + 偏移）。

**为什么纪元换算是本节第一号坑**：NTP 纪元是 **1900-01-01**，Unix 是 **1970-01-01**，
相差 **2208988800 秒（0x83AA7E80）**；NTP 时间戳是 **32.32 定点**
（高 32 位整秒、低 32 位小数）。少加这个偏移量的后果不是"差一点"，
而是**整整 70 年** —— 而且 SOE 时标一旦错 70 年，现场根本不会怀疑是对时算术写错了。
所以常量做成 `static_assert`（把 2208988800 与 0x83AA7E80 一起钉住），
换算路径有 T31/T32 硬断言。

**为什么 `ClockSync` 的基钟必须是单调钟**：如果以**墙钟**为基准再加偏移，
每次对时都在改基准本身，回拨检测就自我循环了。
用 `steady_clock` 做基准 + 一次性偏移，才能做到"墙钟只可能前进"。
T38 钉住"应用一个会让墙钟倒退的偏移时必须被钳住"，
T39/T40 钉住"反复对时（含连续回拨请求）后 `monotonic_violations()==0"`。

**为什么 delay 与 offset 的期望值不能凭直觉写**：
`offset = ((T2-T1)+(T3-T4))/2 = offset_true + 出向延迟 + 处理/2 - 往返/2`。
也就是说，**只有往返时间等于 `2×出向延迟 + 处理时间` 时 offset 才是无偏的** ——
单侧路径不对称会直接污染 offset。这条残差公式写进了 T33 的注释与断言，
并且假服务端把出向延迟、处理时间做成**可注入参数**（否则偏移的符号都验不出来）。
T35 用真实 UDP（端口 12345）端到端验收，T37 覆盖长度/mode/stratum/LI/延迟上限/超时/回错包。

### 2.4 `src/modbus_rtu.h` — Modbus RTU（串口）

**做了什么**：CRC16/Modbus（反射多项式 `0xA001`、初值 `0xFFFF`、**线上低字节在前**）、
请求/响应 PDU 构造、`RtuFramer`（字节流 → 帧）、`ISerialPort` 抽象
（`WinSerialPort` 走 `CreateFile("\\\\.\\COMx")` + `DCB`）、`RtuMaster` 事务。

**为什么分帧把"静默间隔"做成函数参数**：Modbus RTU 的帧边界是
**3.5 个字符时间的静默**。真串口时序在单元测试里造不出来，
但把 API 设计成 `feed(const uint8_t*, size_t, int gap_ms)`（gap_ms 是本批字节**之后**
线路的空闲时长）之后，"半包不给 gap / 给够 gap"就退化成普通函数入参 —— 可精确断言。
T52（整帧 1 次喂）与 T53（分两次喂但不给够 gap → **不**收帧）这一对
才是"分帧器真的在用 gap"的证据。

**为什么分帧必须做"逐偏移重同步"**：这是本项目实测抓到的**真实现缺口**。
第一版 `flush()` 只在"某候选长度刚好放下"或"off→末字节恰好是一帧"时尝试，
于是"垃圾前缀 + 合法帧 + 垃圾尾巴"这种真实噪声形态会把中间的合法帧一起丢掉（T56 变红）。
修法是对**所有偏移**扫描候选长度，并单独接受"off→末字节恰好 CRC 合法"的短帧。

**为什么 CRC 要用外部实现交叉核对**：自己算 CRC 自己验，等于自己批改自己的卷子。
`01 03 00 00 00 0A` 的 CRC 已与本机 `pymodbus 3.15.0` 的 `FramerRTU.compute_CRC`
交叉核对为 `0xCDC5`（线上字节 `C5 CD`），并另核了 5 组向量（T50 全部钉住）。

### 2.5 `src/ftp.h` — FTP 定值下发通道

**做了什么**：最小 FTP 客户端（控制连接 + 数据连接），支持
`USER/PASS/TYPE I/PASV/RETR/STOR/QUIT`，解析多行应答与 `227 (h1,h2,h3,h4,p1,p2)`。

**为什么本模块对 FTP 的"分段应答"如此敏感**：定值下发是**危险动作**。
若客户端在收到 `150` 之后就把"数据到手"当成成功、不等 `226`，
那么一次半截传输会被上层记成"下发成功" —— 现场表现是"参数看起来下了，实际没生效或只生效一半"。
T75 专门让假服务端**不发 226**，断言 `retrieve()` 必须**失败**，
同时反向守卫"数据其实 10 字节都收到了"（证明失败原因真的是等不到 226，而不是没收到数据）。

**为什么多行应答要在 `connect()` 里就整条读掉**：`220-` 是续行、`220 ` 才是结束。
只读第一行的话，后续 `USER` 的应答会对上 banner 的第二行，
于是"每条命令都回奇怪的东西"且**不报错**。T72 除了断言 `banner_lines()==3`，
还断言"确实发过 3 条命令"与"服务端确实处理了 3 条"（反向守卫），
并用**单行 banner 的第二个服务端**断言 `banner_lines()==1` 来反证"不是在数命令"。

### 2.6 `src/tls.h` — 传输层安全

本节结论请看 **§6**（TLS 现状、SChannel 接入步骤、OpenSSL 的许可与供应链决策）。
这里只讲设计原则：

**★ 本文件的第一原则：不假装成功。** TLS 有"看起来能用"的中间态 ——
握手走完、字节能收发，但**证书没校验**。那种实现比完全不支持 TLS 危险得多，
因为它会让人以为通道可信，从而敢把定值下发与遥控命令放上去。

所以 `ITransport` 的 `is_encrypted()` 语义被钉死为
"**握手真的完成之后**才 true"（T83 断言构造完立刻是 false），
而"没有后端"时 `connect()` 返回失败 + 中文原因（T82 逐字钉住原因串），
绝不静默退化成明文。

---

## 3. 踩坑记录（本节是本工程最看重的一节）

每条写清：**现象 / 根因 / 修法 / 判据有效性**。

### 坑① ★ `.bat` 里"中文紧邻 `!VAR!`"会把变量静默吃掉

- **现象**：`build_test.bat` 末尾本该打印 `（3898 条：3094 + 193 + ...）`，
  实际打印成 `（!TOTAL! 条：!P1! + ...）` —— 变量**原样**输出，且 `!`、空格、`+`
  全部消失。脚本**退出码仍是 0**，前面五层全绿，只有这一行坏掉。
- **根因**：`.bat` 是 UTF-8，cmd 按 **CP936** 逐字节解码。
  全角字符 `（` 的 UTF-8 是 `EF BC 88`：`EF BC` 被配成一个 GBK 汉字，
  剩下的 `88` 是合法的 GBK **前导字节**，于是它与紧随其后的 `!`(`0x21`)
  被当成一对**一起吃掉**。后续字节错位，`!TOTAL!` 的开引号没了，
  变量自然不展开。这与 `新模块开发约定.md` §4.3 的"吞 CR"是**同一个机制**，
  只是受害字符从行尾 `\r` 换成了行中的 `!`。
- **修法**：**含 `!VAR!` 的行保持纯 ASCII**；中文只出现在不含变量的 echo 行。
  同时把所有"多条语句分支"从 `if (...) (...)` 改成 `goto` + 标签 ——
  块结构里的中文/`&`/`)` 一旦被错位解码，脚本可能**什么都没做就退出 0**。
- **判据有效性**：修完后同一行打印出
  `合计 PASS=3898 FAIL=0 SKIPPED=0` 与
  `逐层 backoff=3094 sntp=193 modbus_rtu=310 ftp=175 tls=126`。
  这一条也反过来说明：**"脚本退出 0"根本不能证明脚本做对了事**。

### 坑② ★ 被 `CreateProcess` 拉起的 Python 子进程"打印了 READY 却连不上"

- **现象**：TLS 层全部 SKIP，原因统一是"10 s 内端口 2443 未监听"，
  但 `build/tls_srv_2443.log` 里**明明**有 `READY 2443 default`；
  `netstat` 显示本端连接停在 `SYN_SENT`，`netstat | grep 2443` 里**没有** LISTENING 行。
- **根因**：脚本里有个"stdin 读到 EOF 就退出"的看门狗，用
  `sys.stdin.isatty()` 判断"是不是交互式终端"。**Windows 上 `isatty(NUL)` 返回真**
  （NUL 是字符设备）。而我把子进程的 stdin 重定向到了 NUL，
  于是看门狗线程立刻读到 EOF → 服务端打印完 `READY` 就自杀 → 端口消失。
  进程死得太快，日志里连异常都没有。
- **定位方法**（可复用）：写了一个"多变量对照探针"，把
  `CREATE_NO_WINDOW` / 重定向 stdout+stderr / 重定向 stdin / 是否关闭父进程句柄
  四个变量组合成 7 种变体，每种都打印 `alive=` 与"第几轮连上"。结果：
  **只要设了 `hStdInput` 的子进程一律 alive=0**，不设的一律 alive=1。根因当场锁定。
- **修法**：① 看门狗改成**显式环境变量开关**
  （`TLS_ECHO_STDIN_SHUTDOWN=1`），不再用 `isatty()` 做启发式判断；
  ② 测试侧对"服务端起不来"保留 `why_` 明细（含最后一次连接错误与日志路径），
  避免下次又只看到一句"连不上"。
- **判据有效性**：修完后同一套 TLS 用例从 `PASS=66 SKIPPED=6` 变成
  `PASS=126 FAIL=0 SKIPPED=0`，其中 T83 的 `connect()==true` 与
  12 字节回显逐位一致是**正面证据**（不是"没有报错"）。

### 坑③ ★ SChannel 的 `dwProtocol` 是**位标志**，不是版本序号

- **现象**：真 TLS 握手已经成功（回显逐位一致），但
  `negotiated_protocol()` 返回 `unknown(0x00000800)`。
- **根因**：第一版按"版本序号"写映射表（4→TLS1.0、8→TLS1.1、0x10→TLS1.2）。
  实际 SChannel 返回的是 `schannel.h` 里的 `SP_PROT_*` **位标志**：
  `SP_PROT_TLS1_CLIENT=0x80`、`SP_PROT_TLS1_1_CLIENT=0x200`、
  `SP_PROT_TLS1_2_CLIENT=0x800`、`SP_PROT_TLS1_3_CLIENT=0x2000`。
  所以 TLS1.2 是 `0x800`，落进了 `default` 分支。
- **修法**：改成"按高位优先做位测试"（不是精确相等 —— 某些 SDK 会同时置
  SERVER|CLIENT 两位，精确相等会漏），并把 `0x200/0x800/0x2000` 三个常量
  在 g++ 8.1 的 MinGW 头里缺失这一事实写进注释、自己做 `#ifndef` 兜底。
- **判据有效性**：T83 的 `EXPECT_STR_EQ(negotiated_protocol(), "TLS1.2")` 现在恒绿；
  且 T86/T87 用"只收 TLS1.1 的服务端必须协商出 TLS1.1"进一步证明
  这个字符串真的来自协商结果，而不是常量。

### 坑④ ★ Modbus RTU 分帧：垃圾前缀会把中间的合法帧一起丢掉

- **现象**：T56 变红 —— `feed(垃圾 + 合法帧)` 之后 `pop()` 拿不到帧。
- **根因**：**真实现缺口**（不是测试写错）。第一版 `flush()` 只尝试两类位置：
  ① 整段缓冲恰好是一帧；② 某个偏移处的"候选长度"刚好放得下。
  "垃圾前缀 + 帧 + 垃圾尾巴"这种真实噪声形态两类都不满足，于是整段被丢弃。
- **修法**：`flush()` 改为**逐偏移扫描** —— 对每个偏移尝试各候选长度，
  并且额外接受"从 off 到缓冲末尾恰好 CRC 合法"的短帧；`emit()` 返回 bool，
  让 `feed()` 的返回值只统计**真正发出去**的帧（地址过滤掉的帧不算，T57）。
- **判据有效性**：T56 现在同时断言 `frames_ok` 增长与 `resyncs` 增长；
  并且 T55 断言"CRC 错帧被丢弃后，后面的合法帧仍能收到" ——
  这才是"重同步真的有效"，而不是"错了就全清空"。

### 坑⑤ SNTP 的 offset 期望值算错（残差公式）

- **现象**：脚本化时钟给 `offset_true = 1.25 s`，断言却量到 `1.2525 s`。
- **根因**：假服务端的模型是"处理时间 0.005 s、出向延迟 0.010 s"，
  于是 `T4-T1 = 0.025 s`（往返）。按
  `offset = offset_true + out_delay + processing/2 - (T4-T1)/2`
  = `1.25 + 0.010 + 0.0025 - 0.0125 = 1.2525`。
  也就是说：**只有往返时间恰好等于 `2×出向延迟 + 处理时间` 时 offset 才无偏**。
- **修法**：把假时钟的起点/终点改成让 `T4-T1` 恰好等于 `0.025 s`，
  并把这条残差公式**写进断言旁边的注释**（T33），
  同时把"路径不对称会污染 offset"列为已知边界。
- **判据有效性**：T35 现在断言 `offset_s` 与 `delay_s` 的量级与符号，
  并且 T37 用"实际往返超过上限"来触发 delay 上限拒绝 ——
  这条之所以能触发，正是因为 delay 量的是往返而不是单程。

### 坑⑥ `inet_pton` 在 g++ 8.1 的 MinGW 头里取不到

- **现象**：`sntp.h` 编译报 `'inet_pton' was not declared in this scope`。
- **根因**：MinGW-w64 这版 `<ws2tcpip.h>` 里只有 `#define InetPtonA inet_pton`，
  没有可达的声明；`_WIN32_WINNT` 提到 0x0600 也救不回来。
- **修法**：地址解析统一走 `getaddrinfo`（与 `13/src/modbus_tcp_client.h` 一致），
  集中收在 `src/net_base.h` 的 `resolve()` 里。
- **判据有效性**：编译本身就是判据；另外 `resolve()` 对
  `127.0.0.1` / 主机名 / 端口回填都有用例覆盖。

### 坑⑦ FTP 的 `last_code()` 停在 150

- **现象**：`store()` 成功返回，但 `last_code()` 是 `150` 而不是 `226`。
- **根因**：**真实现缺口**。banner / `cmd()` / 数据传完的收尾应答
  三条路径各自记账，其中只有 `cmd()` 会更新"最后一条应答"。
  于是可观测值与事实不一致 —— 半截状态骗人。
- **修法**：抽出 `note_reply()`（落 transcript + 计多行 + 更新 last），三条路径统一走它。
- **判据有效性**：T73/T75 断言"收尾必须停在第 226 条"，
  注释里写明理由："若 226 没读掉，下一条命令会对上它" ——
  这条断言同时守住了"两条应答都被读掉、没残留在流里"。

### 坑⑧ Windows 上 `SO_REUSEADDR` 允许抢占**正在监听**的端口

- **现象**：T72 里"单行 banner"的反证块失败：第二个服务端明明设了
  `banner_multiline=false`，客户端 `banner_lines()` 仍是 3。
- **根因**：Windows 的 `SO_REUSEADDR` 语义与 Unix 不同 —— 它允许
  **绑到一个正在被监听**的端口。两个服务端同时 `listen(2121)` 时，
  客户端连到哪一台是不确定的（本例连回了第一台）。
- **修法**：测试里**先 `stop()` 掉第一台**再起第二台，并在注释里写明原因。
- **判据有效性**：反证块现在稳定拿到 `banner_lines()==1`。
  另外这条也解释了为什么测试里的端口都要挑不重复的
  （TLS 用 2443/2445/2447，SNTP 用 12345，FTP 用 2121）。

---

## 4. 测试清单（五层，职责不重叠）

| 层 | exe | 用例 | 覆盖 | 关键断言 |
| --- | --- | --- | --- | --- |
| 1 | `test_backoff_arbiter.exe` | T01~T20 | 退避序列 / 抖动 / 确定性 / 归一 / 双链路滞环 | T05 固定间隔逐次恒等；T12 N-1 不切第 N 才切；T14 切换次数上界 + 无滞环对照；T19 契约检测器有区分度 |
| 2 | `test_sntp.exe` | T30~T40 | 报文编解码 / 纪元 / 定点 / offset-delay / 回拨钳位 | T31 2208988800；T33 残差公式；T38 回拨被钳住；T40 `monotonic_violations()==0` |
| 3 | `test_modbus_rtu.exe` | T50~T65 | CRC / 分帧 / 重同步 / 假串口 / 主站事务 | T50 6 组 CRC 已知答案（已与 pymodbus 核对）；T53 gap 不够不收帧；T56 垃圾前缀重同步；T64 参数越界必须失败 |
| 4 | `test_ftp.exe` | T70~T78 | 应答解析 / PASV / 多行 banner / 收发逐位对账 | T73/T74 逐位一致；T75 无 226 必须失败；T71 端口 0 与越界必须拒 |
| 5 | `test_tls.exe` | T80~T89 | 明文对照 / 显式拒绝 / 真 TLS / 证书校验 / 版本协商 | T81 TLS 打明文口必须失败；T82 拒绝原因逐字钉住；T83 真回显；**T84 自签必须被拒**；T87 `tls12_only` 可观测 |

**两层以上的反证（判据有效性）**：本项目最硬的纪律是
"把实现故意改错，这条断言会红吗"。本模块做了 6 次反证：

1. T02：抖动**确实在起作用**（否则抖动恒为 1 也能过"落在区间内"）。
2. T09：合法 policy **不**被归一（否则 T06 可以被"无脑归一"骗过）。
3. T14：无滞环的对照实现在同一序列下切换 ≥10 次（否则"次数上界"可能只是条件没触发）。
4. T36/T31：反向守卫栏里写清"哪一条是在证明上一条测到了东西"。
5. T72：单行 banner 的第二个服务端反证 `banner_lines()` 不是在数命令。
6. T84：关掉证书校验后**同一台服务端**必须连得上（否则"被拒"的原因就不是证书）。

### 4.1 断言总数有两个合法口径（**报告时必须说清用的哪个**）

| 口径 | 条件 | 逐层 | 合计 |
| --- | --- | --- | --- |
| A | 环境里有 python（TLS 层端到端真跑） | 3094 / 193 / 310 / 175 / **126** | **PASS=3898 FAIL=0 SKIPPED=0** |
| B | 没有 python（TLS 层 6 条显式 SKIP） | 3094 / 193 / 310 / 175 / **68** | **PASS=3840 FAIL=0 SKIPPED=6** |

判据：`build_test.bat` 末行。口径 B 下脚本**不会**印"五层全部通过"，
而是印 `[SKIP]` 并回显"TLS 端到端断言未验证" ——
否则"静默跳过"与"真的跑过"在输出上不可区分，这正是本项目最忌讳的一类失败。
`test_tls.exe` 自己的"TLS 状态"行也会跟着变（有跳过时不再印"端到端已验"）。

---

## 5. 怎么跑

```bat
REM 只编译（冒烟检查，不跑用例）
16\scripts\build.bat

REM 编译 + 逐层运行 + 汇总
16\scripts\build_test.bat
```

**不要从 Git Bash 直接跑**（仓库路径含括号）—— 用 Python 起子进程：

```python
import subprocess
r = subprocess.run(['cmd', '/c', 'build_test.bat'],
                   cwd=r'D:\wb(cn)\BMS\16\scripts', capture_output=True)
print(r.returncode)
print(r.stdout.decode('utf-8', 'replace'))
```

环境变量 `EMS_PYTHON`：指向 python.exe（只有 TLS 层用）。
**一旦设置就以它为准，不做静默回退** —— 设错路径却还被 PATH 上的 python
悄悄顶替，会让"我明明指定了那个解释器"变成假话。

---

## 6. TLS 现状、SChannel 接入步骤、OpenSSL 决策

### 6.1 ★ 现状（一句话，不含糊）

**Windows 上本模块的 TLS 是"真的"：走 SChannel（操作系统组件），
与 Python `ssl`（OpenSSL 3.x）服务端端到端握手成功并逐位回显一致，
证书校验默认开启且"自签证书必须被拒"这条有硬断言。
非 Windows 平台没有编译进任何后端，`connect()` 显式拒绝、绝不退化成明文。**

对应的断言（不是"没有报错"，是正面证据）：

| 证据 | 用例 |
| --- | --- |
| 与**别人的** TLS 实现（OpenSSL）握手成功 | T83 `connect()==true` |
| 加密语义为真：`is_encrypted()==true`，且构造完那一刻是 false | T83 |
| 12 字节 / 256 KiB 明文经 TLS 往返**逐位一致**（跨多条 TLS 记录） | T83 / T85 |
| 协商结果来自真实协商：`negotiated_protocol()=="TLS1.2"`，多轮握手 `rounds>=3` | T83 |
| 对端证书主体真的读到了（`localhost`） | T83 |
| **证书校验确实开着**：自签证书被 `SEC_E_UNTRUSTED_ROOT` 拒绝 | T84 |
| 校验不是被默认关掉的：关掉校验后同一台服务端连得上 | T84 反向守卫 |
| 版本协商不是摆设：只收 TLS1.1 的能连出 TLS1.1、只收 1.3 的连不上 | T86 |
| `tls12_only` 的窄化可观测 | T87 |
| 无后端时显式拒绝、原因串逐字钉住 | T82 |
| 把 TLS 打到明文口必须失败，且失败后不得自称"已加密" | T81 |

### 6.2 为什么选 SChannel 而不是 OpenSSL

- SChannel 是**操作系统组件**，随 Windows 一起到场 —— 不破坏"零第三方依赖"纪律。
- OpenSSL 会引入两个**非技术**决策：
  1. **许可**：OpenSSL 3.x 是 Apache-2.0（与 1.x 的双许可不同），
     需要法务确认与本工程 GPL-3.0 的兼容性；
  2. **供应链**：多一个需要持续打补丁的原生库，现场量产机器上要跟 CVE。
- 代价（诚实列出）：SChannel 实现是 Windows 专有，Linux 构建下本模块只能
  `Backend::kNone`；且它的 API 是 SSPI 那套
  "`InitializeSecurityContext` 循环 + `EncryptMessage`/`DecryptMessage`"，
  比 OpenSSL 的 BIO 抽象啰嗦得多。

### 6.3 接入步骤（照做即可，已在 g++ 8.1 + MinGW-w64 上实测跑通）

1. `#include <security.h>` / `<schannel.h>` / `<sspi.h>`，
   并在其**之前**定义 `SECURITY_WIN32`；`<winsock2.h>` 必须在 `<windows.h>` 之前
   （这条链由 `net_base.h` 保证）。
2. MinGW-w64 这版 `schannel.h` 只到 TLS1.0，缺
   `SP_PROT_TLS1_1_CLIENT(0x200)` / `SP_PROT_TLS1_2_CLIENT(0x800)` /
   `SP_PROT_TLS1_3_CLIENT(0x2000)` —— 自己 `#ifndef` 兜底。
3. `AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, ...,
   &SCHANNEL_CRED{...})`。`dwFlags = SCH_CRED_NO_DEFAULT_CREDS` 即"校验证书"；
   只有在显式要求不校验时才追加
   `SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK`。
4. `InitializeSecurityContextA` **循环**（多轮），处理三种返回：
   `SEC_I_CONTINUE_NEEDED`（继续）、`SEC_E_INCOMPLETE_MESSAGE`（**输入未被消费，
   不能丢 `raw_` 缓冲**）、`SEC_OK`（完成）。`SECBUFFER_EXTRA` 里的尾巴要留着。
5. 完成后 `QueryContextAttributesA(SECPKG_ATTR_STREAM_SIZES)` 拿分片尺寸，
   发数据按 `cbMaximumMessage` 切段、`EncryptMessage`；收数据 `DecryptMessage`
   并处理 `SECBUFFER_EXTRA` 与 `SEC_I_CONTEXT_EXPIRED`。
6. 链接 `-lsecur32 -lcrypt32 -lws2_32`。
7. `SECURITY_STATUS` 一律翻中文再上报（**未收录的码也要原样打出来**，
   否则现场只看到"握手失败"四个字，完全没法查）。

### 6.4 13/ 的接缝：重连退避策略（**本阶段没有改动 13/**）

13/ 的重连目前是固定间隔（`13/src/modbus_device_io.h` 的
`Config::reconnect_interval_ms = 1000`，第 77 行；`try_reconnect()` 第 477 行）。
`Backoff` 在 `factor=1.0 & jitter_ratio=0` 时逐次恒等于 `initial_ms`（T05 硬断言）
就是为这次替换准备的接缝。

**本阶段刻意没有改 13/**，理由三条（都有依据，不是怕麻烦）：

1. `modbus_device_io.h` 要 `#include "backoff.h"`，就必须在
   `13/scripts/build_test.bat` 的 `INC` 里加 `-I ..\16\src` ——
   而 13/ 的构建脚本本轮**不允许改动**。加了字段却用不上，等于挂了一个
   **假能力**（比不做更糟，正是本模块最反对的那种失败）。
2. 依赖方向会反过来：13/ 是**产品模块**，16/ 是**工程加固模块**。
   让 13/ 依赖 16/ 会把 13/ 的构建绑到 16/ 上，与"加固模块并列在上层"的分层不符。
3. 正确顺序是由集成方**一次性**完成"挂 `build_all.bat` 步号 + 加 `-I` + 改配置"，
   避免多人并发改同一个构建树互相覆盖（约定 §6）。

**将来要做的最小改动**（供集成方参考）：

```cpp
// 13/src/modbus_device_io.h  Config 内（新增，默认值保证行为不变）
bool                          reconnect_backoff_enabled = false;   // 默认关
ems::comm::BackoffPolicy      reconnect_backoff;                   // 默认 initial=1000/max=30000/factor=2/jitter=0.2
ems::comm::Backoff            backoff_;                            // 私有成员

int reconnect_wait_ms() const {
    if (!cfg_.reconnect_backoff_enabled) return cfg_.reconnect_interval_ms;  // 原路径，逐位不变
    return backoff_.peek_next_delay_ms();
}
// try_reconnect() 里的 cfg_.reconnect_interval_ms 换成 reconnect_wait_ms()，
// 连接成功时 backoff_.reset()，失败时 backoff_.next_delay_ms() 推进一次。
```

---

## 7. 已知边界（**诚实列出，没做的就写没做**）

1. **TLS 只在 Windows 上有后端**。Linux/交叉编译下 `resolve_backend()` 返回
   `kNone`，`connect()` 显式拒绝。这是选择 SChannel 的直接代价，不是遗漏。
2. **TLS 不做服务端**（EMS 是客户端）、**不做会话恢复/重新协商**
   （`SEC_I_RENEGOTIATE` 直接报错）、**不做客户端证书（mTLS）**、
   **不做 OCSP/CRL 吊销检查的显式控制**（交给系统默认）。
   本工程无对外服务场景，这些都不在需求内；但如果将来要接"设备反向连我"，
   mTLS 与 server-side SChannel 都要补。
3. **TLS 不做 SNI 之外的主机名策略**：证书主机名校验交给 SChannel 的
   target name。若目标机证书 CN 与现场 IP 不一致，需要显式配置 `sni_host`。
4. **TLS 的 1.3 默认不开**：`tls12_only=false` 时提供 TLS1.0/1.1/1.2。
   不开 1.3 的原因：① 现场老盒子/中间盒对 1.3 支持不全；
   ② SChannel 的 1.3 要 Win11/Server2022 起才有，老机器上置这一位可能直接协商失败。
   要放开必须先评估目标机，且 1.0/1.1 应当同时关闭。
5. **SNTP 的 2036 年回绕**：`ts_to_unix_seconds()` 用固定 `+2208988800` 偏移，
   在 2036-02-07 之后无法区分纪元（T31 明确钉住了这个行为）。
   `ts_diff_seconds()` 做了 64 位环绕安全做差，所以**差值**在回绕附近仍然正确 ——
   本模块实际只用差值。真到了 2036 年需要按 RFC 4330 的 era 规则重写这一段。
6. **SNTP 不校正路径不对称**：`offset` 的无偏性依赖
   `往返 = 2×出向延迟 + 处理时间`。单侧拥塞会把 offset 拉偏，
   本模块只做"延迟超上限就拒绝"（`max_delay_s`），不做对称性估计。
7. **SNTP 不做认证（NTS）**：报文无签名，链路上可被伪造。
   内网专线场景可接受，公网不可接受。
8. **Modbus RTU 只用假串口验收**：`WinSerialPort` 的 `DCB` 配置路径
   **没有在真串口上跑过**（本机无串口设备）。波特率/校验/停止位的映射
   是照 Windows API 语义写的，但没有实测证据 —— 首次上真机必须单独验这一条。
9. **Modbus RTU 不做 ASCII 模式 / 不做诊断码（FC 08）/ 不做广播写**。
10. **Modbus RTU 的 3.5 字符时间下限是 2 ms**：高波特率（≥115200）时
    理论上 3.5 字符时间小于 1 ms，实现里抬到 2 ms 以避开定时器分辨率。
    这会让高波特率下的帧边界判定比标准**更宽松**，属于有意的工程折中。
11. **FTP 明文 + 无 FTPS**：RFC 4217（FTPS）没做。
    定值下发的机密性目前**不在 FTP 通道上保证** ——
    真要保就得上 FTPS 或走 `TlsTransport` 包一层隧道。
    这是本模块最值得下一步做的事（定值是危险载荷）。
12. **FTP 不做 IPv6 / 不做 EPSV / 不做主动模式（PORT）**：
    只实现被动模式（现场 NAT 后能用的那一种）。
13. **FTP 的 `max_data_bytes` 默认 64 MiB**：超过就报错而不是截断，
    但"服务器一直发"的检测是超限后才报，不是流控。
14. **双链路仲裁是"探测式"的**：每拍都会对未 up 的链路主动 `connect()`。
    这是有意的（现场设备不会主动通知我们它回来了），
    但代价是探测本身要耗时间 —— 上层必须保证 `step()` 的调用节奏。
15. **双链路只支持主备（1+1），不支持 N 选 1 或链路聚合**。
16. **本模块还没挂进 `scripts/build_all.bat`**：按约定 §6，
    这一步由集成方统一做（避免并发改同一文件）。步号建议排在 16\。
17. **`.bat` 的 console 输出有少量中文显示错位**（CP936 解码 UTF-8 的必然结果），
    属于**显示层**问题：所有被解析的数字都在纯 ASCII 行上，
    落盘的 `build\*_status.txt` 也是完整正确的。
18. **`build/` 不入库**。排查坑②时用的多变量对照探针（`spawn_probe` / `proto_probe`）
    与一次性头文件检查都是**过程性工具**，本轮已删除，不构成本阶段成果 ——
    它们的作用已经写进 §3 坑②的"定位方法"里，可照原样重建。

---

## 8. 目录结构

```
16/
├── src/                    实现（全部 header-only，inline）
│   ├── net_base.h          ★ 内部共享：Winsock 引导、TcpSocket / TcpListener、
│   │                         getaddrinfo 解析、recv/send 超时
│   │                         （"内部共享头"，不是独立成果）
│   ├── backoff.h           退避策略（确定性 PRNG + 归一）
│   ├── link_arbiter.h      主备双链路仲裁（SFINAE 契约检测）
│   ├── sntp.h              SNTP 客户端 + ClockSync（回拨钳位）
│   ├── modbus_rtu.h        CRC16 / 分帧 / 串口抽象 / 主站事务
│   ├── ftp.h               FTP 定值通道 + PASV 解析
│   └── tls.h               ITransport / PlainTransport / TlsTransport(SChannel)
├── tests/
│   ├── test_backoff_arbiter.cpp   T01~T20
│   ├── test_sntp.cpp              T30~T40
│   ├── test_modbus_rtu.cpp        T50~T65
│   ├── test_ftp.cpp               T70~T78
│   ├── test_tls.cpp               T80~T89
│   └── fixtures/
│       ├── tls_echo_server.py     TLS 回显服务端（Python ssl / OpenSSL）
│       ├── echo_cert.pem          自签证书（CN=localhost，SAN=DNS:localhost,IP:127.0.0.1）
│       └── echo_key.pem           对应私钥
├── scripts/
│   ├── build.bat           只编译（冒烟检查）
│   └── build_test.bat      编译 + 逐层运行 + 汇总（两个合法口径之一）
├── docs/
│   └── README.md           本文件
└── build/                  产物（.exe / *_status.txt），不入库
```

**编译参数**

```
头文件包含路径：  -I src
分层链接：
  backoff / sntp / modbus_rtu / ftp   -lws2_32
  tls                                 -lws2_32 -lsecur32 -lcrypt32
标准：-std=c++17 -Wall -O2
```

**依赖方向**：`16/ → {自身, Windows 系统库(ws2_32/secur32/crypt32)}`。
零第三方代码依赖；跨模块 `#include` 走 `-I src` 的扁平解析。

**已知工具链约束**（g++ 8.1）：避开 `<filesystem>`（需另链 `-lstdc++fs`）
与 `<charconv>`（整型转换不完整）；用 `<string>` + 手写转换。
