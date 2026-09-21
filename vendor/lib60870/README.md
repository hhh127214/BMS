# lib60870（第三方库，原样引入）

本项目用它对上实现 **IEC 60870-5-104 从站（服务器）**，把 EMS 的实时数据上送电网调度 /
虚拟电厂平台，并接收调度下发的遥控与遥调。网关代码在 `P3/`。

> 上游自带说明已改名为 [`README-upstream.md`](./README-upstream.md)；官方手册源文件是
> [`user_guide.adoc`](./user_guide.adoc)。

---

## 1. 来源与版本

| 项 | 值 |
|---|---|
| 上游仓库 | <https://github.com/mz-automation/lib60870> |
| 上游路径 | 仓库根下的 `lib60870-C/` |
| 版本 | **2.4.1**（`CMakeLists.txt` 的 `LIB_VERSION_MAJOR/MINOR/PATCH`） |
| 引入时 commit | `7a388e3e133999e1ca77ba7521d55d074b7cd2bc`（2026-07-15，`Merge branch 'v2.4_develop'`） |
| 引入方式 | 源码纳入（`git clone --depth 1` 后取 `lib60870-C/`） |
| 语言 | 标准 **C99**（不是 C++ 库，但头文件带 `extern "C"`，C++ 可直接 include） |
| 授权 | **GPL-3.0** 或商业授权双轨 —— 见 §5 |

### 相对上游做的删减

只删了与运行无关的东西，**没有改任何一行源码**：

| 删除项 | 原因 |
|---|---|
| `.git/` | 不做子模块，避免嵌套仓库 |
| `doxydoc/`、`Doxyfile` | 108 KB 的文档生成配置，运行不需要 |
| `tests/` | 716 KB 上游自己的单元测试，我们不跑它 |

保留 `examples/`（275 KB）—— 它是**最好的用法参考**。

### 目录说明

```
vendor/lib60870/
├── config/lib60870_config.h     ← 编译期配置（队列大小 / 线程 / 最大连接数）
├── src/
│   ├── inc/api/                 ← ★ 只有这里的头文件需要被我们的代码 include
│   │   ├── cs104_slave.h        ←   从站（服务器）API —— 本项目主用
│   │   ├── cs104_connection.h   ←   主站（客户端）API —— 本项目测试用（自环）
│   │   ├── cs101_information_objects.h ← 信息对象构造/读取
│   │   ├── iec60870_common.h    ←   TypeID / COT / ASDU / CP56Time2a
│   │   └── iec60870_slave.h     ←   IMasterConnection（回调里回送数据用）
│   ├── inc/internal/            ← 库内部头（编译时需要 -I，我们不用 include）
│   ├── common/inc/              ← linked_list 等
│   ├── hal/inc/                 ← HAL 头（hal_time.h / hal_thread.h / hal_socket.h）
│   ├── iec60870/                ← 协议实现（cs101 / cs104 / link_layer / apl）
│   ├── hal/{socket,thread,time,serial,memory}/
│   └── file-service/
├── examples/                    ← 上游示例（建议读 cs104_server / cs104_client_async）
├── user_guide.adoc              ← 上游官方手册源文件
├── COPYING                      ← GPL-3.0 全文
├── CHANGELOG
└── build.bat                    ← ★ 本项目写的构建入口（见 §2）
```

---

## 2. 怎么编

```bat
vendor\lib60870\build.bat
```

产物 `vendor/lib60870/build/lib60870.a`（静态库，约 300 KB）。

上游自带的 `Makefile` / `CMakeLists.txt` **本项目不用**，原因写在 `build.bat` 头部：
本机没有 `make`；上游 CMake 会连带编 `examples/` `tests/` 并把 TLS(mbedtls) 拉进来，
而我们只用明文 104，裁剪后 23 个 `.c` 就够。

### 编译参数（必须照抄）

| 项 | 值 | 为什么 |
|---|---|---|
| 编译器 | `gcc`（**不是** `g++`） | 上游是 C99 代码 |
| 标准 | `-std=gnu99` | 用了 GNU 扩展（`__attribute__` 等） |
| 包含路径 | `-Iconfig -Isrc/inc/api -Isrc/inc/internal -Isrc/common/inc -Isrc/hal/inc -Isrc/file-service` | 缺一不可 |
| 链接（**使用者**加） | `-lws2_32 -liphlpapi -lbcrypt` | win32 HAL 的 socket / 网卡枚举 / 随机数 |

`bcrypt` 是 MinGW 下最容易漏的一个 —— 漏了会报 `undefined reference to BCryptGenRandom`。

### win32 HAL 的源文件选择

上游 `src/CMakeLists.txt` 按平台分四组（linux / windows / bsd / macos）。本机取：

```
lib_common_SRCS（17 个，协议层，跨平台）
  + common/linked_list.c（1 个）
  + lib_windows_SRCS（5 个：socket_win32 / thread_win32 / time / serial_port_win32 / lib_memory）
= 23 个
```

编 linux / bsd / macos 那三组会重复定义符号。

---

## 3. 实测基线（本机 MinGW-W64 gcc 8.1.0）

| 项 | 结果 |
|---|---|
| 编译 | 23/23 成功，**0 error**；2 个 `-Wunused-*` 警告来自上游自身代码，无害 |
| 静态库 | `build/lib60870.a` |
| 冒烟 | `CS104_Slave_create` → `alPar{CA=2, IOA=3}`、`apci{k=12,w=8,t0=10,t1=15,t2=10,t3=20}`、`TypeID_toString`、`CP56Time2a` 往返 全部正常 |

---

## 4. 四处与手册不一致 / 手册未提到的点

> 全都是实测出来的，不是从文档抄的。**照手册写就会错的地方。**

### 4.1 从站默认 `t0 = 10`，手册写 30

`src/iec60870/cs104/cs104_slave.c:90-95` 的默认值块：

```c
/* .k = */ 12,  /* .w = */ 8,
/* .t0 = */ 10, /* .t1 = */ 15, /* .t2 = */ 10, /* .t3 = */ 20
```

`t0` 是**连接建立超时**。手册表格里的 30 是规范推荐值，代码默认给的是 10。
按手册调参前先 `CS104_Slave_getConnectionParameters()` 读一次实际值。

### 4.2 `CP56Time2a` 用的是 **UTC**，不做时区换算

`src/iec60870/apl/cpXXtime2a.c:477` 的 `CP56Time2a_setFromMsTimestamp()`：

```c
time_t timeVal = timestamp / 1000;
struct tm tmTime;
#ifdef _WIN32
    gmtime_s(&tmTime, &timeVal);     /* ← gmtime：UTC，不是 localtime */
#else
    gmtime_r(&timeVal, &tmTime);
#endif
```

也就是 `CP56Time2a_createFromMsTimestamp(t, Hal_getTimeInMs())` 写进去的是 **UTC** 时标。

**这对国内调度是错的** —— 国内要求 CP56Time2a 是**北京时间**（= UTC+8，夏令时位置 0）。
实测本机：

```
Hal_getTimeInMs = 1789811518735
CP56Time2a      = 2026-09-19 09:51:58.735     ← UTC
本机墙钟        = 2026-09-19 17:51:58.735     ← UTC+8
```

**这个坑的可怕之处是自环测试照不出来**：主站侧的 `CP56Time2a_toMsTimestamp()` 也按 UTC
解释，两边口径一致地错 → 往返恒等，测试全绿。但调度主站拿到的时标会差 8 小时。

处理方式见 `P3/src/iec104_time.h`（显式做本地↔UTC 换算），**不要**依赖库的默认行为。

### 4.3 总召唤的 `ACT_TERM` **必须带一个信息对象**（手册漏写）

这一条是**唯一一个"完全照手册写就必然错"**的点，而且症状会把你引到错误的方向。

总召唤是异步的：回调只登记 + 回 ACT_CON，数据在 `tick()` 里分批推，最后发 ACT_TERM 收尾。
直觉上"终止帧没有数据"，于是会去创建一个**空 ASDU**：

```c
CS101_ASDU a = CS101_ASDU_create(params, false,
                                 CS101_COT_ACTIVATION_TERMINATION, 0, ca, false, false);
IMasterConnection_sendACT_TERM(con, a);       /* ← 错的 */
```

**空 ASDU 的 TypeID 是 0**，而主站侧 `CS101_ASDU_createFromBufferEx()` 会校验 TypeID
合法性 → 直接丢帧。结果是：

```
主站侧：等不到 ACT_TERM → 超时重召 → 再等不到 → 再重召 …（看起来像主站有病）
从站侧：gi_requests 一直涨，一切"正常"
```

正确写法是给终止帧装一个总召唤命令信息对象 —— 官方
`examples/cs104_server/simple_server.c` 就是这么写的（手册 §6.4/§6.9 的示例代码漏了）：

```cpp
if (InformationObject io = reinterpret_cast<InformationObject>(
        InterrogationCommand_create(nullptr, 0, qoi))) {
    CS101_ASDU_addInformationObject(a, io);
    InformationObject_destroy(io);       /* addInformationObject 内部已复制 */
}
IMasterConnection_sendACT_TERM(con, a);
```

本项目 `P3/src/iec104_server.h::serve_gi()` 按后者实现，并加了统计项
`gi_act_term_sent` 以及测试断言（收到 ACT_CON 但没有 ACT_TERM 就报警）。

### 4.4 带时标的类型**没有公开的取值接口**

`M_ME_TF_1(36)` / `M_SP_TB_1(30)` 这类带 CP56Time2a 的信息对象，
`cs101_information_objects.h` 只给了 `_create` / `_destroy` / `_getTimestamp`，
**没有 `_getValue` / `_getQuality`**（而对应的不带时标版本都有）。

从站侧发得出去（构造完整），但**主站侧要读值就只能 include 内部头**
`src/inc/internal/information_objects_internal.h`，再按结构体直接读字段：

```cpp
struct sMeasuredValueShortWithCP56Time2a* s =
    reinterpret_cast<struct sMeasuredValueShortWithCP56Time2a*>(io);
double v = s->value;   int q = s->quality;
```

所以编译对端工具（主站模拟器）要额外加 `-I .../src/inc/internal`；
**生产代码不需要**。这也是"对端工具"与"产品代码"的合理分界：
工具为了验证可以碰内部结构，产品不行。

---

## 5. ⚠️ 授权：GPL-3.0 或商业授权

上游 `README-upstream.md` §Licensing 原文：

> This software can be dual licensed under the GPLv3 and a commercial license agreement.
> When using the library in commercial and non-GPL applications you should buy a commercial license.

两条路：

1. **GPL-3.0**：可以免费用，但整个衍生作品必须开源。
2. **商业授权**：向 MZ Automation 购买，可闭源分发。

**静态链接**（本项目的做法）在 GPL 下通常被视为构成单一作品，传染范围比动态链接更大。
**这是产品化决策，需要商务/法务确认**，我在代码层面只做了"库与业务代码物理隔离"
（`vendor/lib60870/` 与 `P3/` 两个目录），方便后续换成商业授权或替换实现。

替换成本很低：`P3/` 里只有 `iec104_server.h` 与 `main_gateway.cpp` 依赖库的 C API，
换成别的 104 栈只需重写这两个文件 —— 点表映射（`iec104_point_map.h`）与时标换算
（`iec104_time.h`）是协议无关的，可以原样保留。
