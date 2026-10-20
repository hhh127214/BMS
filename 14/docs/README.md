# 14/ — 平台层后端 + SQLite 数据库

> **一句话**：把 EMS 的数据装进数据库、对上开 REST 接口，让
> 「这个站现在怎么了、今天赚了多少、有没有越限」不用再去翻 8 万行时序 CSV。

对应设计方案 [`docs/architecture.md`](../../docs/architecture.md) 的
**§3.1 第四层「平台层」**、**§22 系统数据库设计**、**§24 E（后端）/ F（前端）** 两岗。

---

## 1. 为什么需要它

`01`–`13` / `P1`–`P3` 已经把**设备层**与 **EMS 核心控制层**做透了（当时 9748 断言 / 33 组件；
补 `16/`~`20/` 后为 **16473 断言 / 41 步**（2026-10-08 四项界面补漏后；`17/` 四个选配模拟器落地时是 16276），
2026-10-18 完成 v1.8「清册并入点表 + 全页弹层」后为 **17453 断言 / 43 步**（本机齐备口径，`14/` 自身 **870**），
  见根 `README.md` 四口径表），
但框架的**第四层平台层此前是空的**：没有数据库、没有接口、没有界面。

结果是：系统能跑、能验收，但**运行人员看不见它在干什么**。要回答"昨天发生了什么"，
只能打开 `10/build/timeseries.csv`（8640 行 × 20 列）。

`14/` 补的就是这一层的第一半：**数据落库 + 对上开接口**。第二半（界面）在 `15/`。

---

## 2. 阶段成果

| 文件 | 作用 |
| :--- | :--- |
| `schema.sql` | 数据库 schema。§22 的六张表 + 时序明细/场景/经济性/不变量/用户/会话/审计 |
| `src/emsdb.py` | 连接、建表、通用查询（WAL 模式：导入不挡读） |
| `src/importer.py` | 把 `10/build` 的真实仿真产物装进库 |
| `src/auth.py` | 口令（pbkdf2）、会话（token）、三角色权限、审计 |
| `src/api.py` | REST 路由与业务逻辑（56 个端点，含引擎、仿真调度、两端接入、上端转发表、模拟链路进程与接入向导草稿）<br>`src/simproc.py` —— 模拟链路的进程编排：替身 ×N / 采集器 / 上端网关 / 模拟主站（v1.10）<br>`src/upmapconf.py` —— 上端 IOA 转发表的校验 / 序列化 / 原子落盘（v1.6 起可保存）<br>`src/accessconf.py` —— **「接入调度主站」九步的字段清单、草稿读写、缺项校验与台账 CSV**（v1.14；九步只在这里定义一份，界面渲染 / 完成校验 / 导出列全从它来） |
| `src/server.py` | HTTP 服务（标准库 `http.server`，同时托管 `15/` 静态页） |
| `tests/selftest.py` | 断言式自检，**664 断言**（`[K]` 段随告警数据集变化，非常量） |
| `scripts/*.bat` | 导入 / 启服务 / 自检 / live 入库 / 一键 |

**零第三方依赖** —— 只用 Python 标准库（`sqlite3` / `http.server`）。
与项目其余部分一致：不要让现场部署先装一堆包。

---

## 3. 快速开始

```bat
rem 1) 先把 10/ 的仿真产物跑出来（若还没有）
10\scripts\run_demo.bat

rem 2) 导入数据库 + 自检
14\scripts\run_all.bat

rem 3) 启服务（默认 127.0.0.1:8765）
14\scripts\run_server.bat
```

然后：

```bat
curl http://127.0.0.1:8765/api/health
curl -X POST http://127.0.0.1:8765/api/login -H "Content-Type: application/json" -d "{\"username\":\"admin\",\"password\":\"admin123\"}"
```

> Python 解释器可用环境变量 `EMS_PYTHON` 指定；未设置时用 `PATH` 里的 `python`。

---

## 4. 数据库

### 4.1 表与数据来源

| 表 | 来源 | 说明 |
| :--- | :--- | :--- |
| `device` | `10/build/summary.json` 的 config/curves | 6 台：PCS / BMS / 电表 / 光伏 / 变压器 / 负荷 |
| `realtime_data` | 同仿真（点级快照） | 值 + **品质位** + 时刻，三要素与 RT_DB 写入模型一致 |
| `strategy_config` | `04/src/strategies_9.h` | **9 个策略**，L0–L3 |
| `control_command` | `10/build/timeseries.csv` | 下发 vs 实际 + 权限区间 + 归因 |
| `alarm` | `10/build/alarms.csv` + `fault/alarms.csv` | normal 7 条 / fault 289 条 |
| `energy_statistics` | 时序积分 + 经济性 | 按日 × 场景 |
| `step_record` | `timeseries.csv` 全 20 列 | 曲线页数据源；两场景各 8640 行 |
| `scenario` / `economics` / `invariant` | `summary.json` | 场景元信息、经济性、安全红线 |
| `app_user` / `session` / `audit_log` | 平台自身 | 用户、会话、审计 |
| `meta` | 导入器 | 记录导入来源与时间 |
| `device_conn` / `upstream_link` | **用户在界面上配**，不走导入 | 两端接入参数（v1.5）：下端**逐台**记「真实源 / 模拟源」，上端记监听口 / 公共地址 / 形态；都会落成端侧进程读的 `key=value` 文件（见 §4.4）<br>**`upstream_point_map`（v1.6）**：上端 IOA 转发表的库存档。与 `active.pointmap.csv` 同源同生 —— 文件是通道（端侧读它），库是存档（导出 / 对账 / 删完能确认清干净） |

### 4.2 两条硬纪律

1. **不另抄点表。** 涉及点名/索引的字段以 `07/src/rtdb/ems_point_table.h` 为准。
   本项目已经因为手抄常量出过一次「24 个点全灰」的事故（见 `P3/docs/README.md` §7.1）。
2. **导入必须幂等。** 重复执行 `import_sim.bat` 不产生重复行、不漂移数值
   （自检 `[D] 幂等` 段专门守这条）。

### 4.3 数据边界（必须知道）

**当前没有真机。** 库里所有现场量都来自 `10/` 的**离线仿真**（24 h / 10 s 粒度）。
这正是本项目"缺真数据就用模拟器"的口径。

因此：

- 平台的"实时" = **仿真时标 `t_s`**，8640 拍可按任意速度回放（前端 1 秒看完 24 小时）；
- 经济性数字（净收益 572.01 元/日、回收期 5.75 年）是**仿真结果**，不是实测账单；
- 等 `13/`（Modbus 主站）或 `P3/`（IEC104 从站）接上真设备后，
  只需把写入端换掉，**表结构与接口不用改** —— 这与 `IDeviceIO` 上"换介质不改算法"是同一个设计。

### 4.4 两端接入（v1.5）：两张表，不是一个表加两列

EMS 在这套系统里**同时扮演两个相反的角色**，这一点决定了表结构：

| 端 | EMS 的身份 | 连几个 | 参数放哪 |
| :--- | :--- | :--- | :--- |
| **上端**（对上调度） | IEC 60870-5-104 **从站**（`P3/`），监听 2404 | **1 个链路**（一个监听口 = 一条链路） | `upstream_link` 单行（`link_id='dispatch'`） |
| **下端**（对下设备） | Modbus TCP **主站**（`13/`） | **N 台**，每台一套参数 | `device_conn` 每设备一行 |

**为什么不合成一张表。** 上端是 1:1 被动监听，下端是 1:N 主动轮询，两边的字段集几乎不相交
（下端有从站号 / 轮询周期 / 超时，上端有公共地址 CA / 监听网卡）。硬塞进一张表就要一半列永远为 NULL，
而"这一列什么时候该有值"就再也说不清了 —— 这正是本项目反复吃过的那类亏。

**下端汇总以 `device` 为左表。** `GET /api/access` 的 `counts`（真实源 / 模拟源 / 未配置）
必须覆盖**台账里的每一台**，而不是只数 `device_conn` 的行 —— 现场最常见的初始状态是
「6 台设备登记好了，接入参数一行都没有」，只数 `device_conn` 会报成「未配置 1」，
而卡片上三个数加起来比台账少，界面上看不出少了谁。所以是
`FROM device d LEFT JOIN device_conn c`，没配过的自然落进 `unset`。

**「形态」不是第二条代码路径。** 两端都跑同一套协议栈，`real` / `sim` 只决定**跟谁说话**：

- 下端 `source_kind='sim'` ⇒ 端侧实际连 `sim_host:sim_port`（默认 `127.0.0.1:1502`，即 `17/` 的高保真模拟器），
  `port`（真实设备端口）**原样保留**，切回 `real` 就恢复；
- 上端 `kind='sim'` ⇒ 由 `P3/build/iec104_master.exe` 扮演调度主站连进来。

**为什么要记、要在界面上看得见。** 调试期用模拟源联调、投运切真源，是标准流程；
危险的是**忘了切回来** —— 那时界面一切正常、数据也在动，因为模拟器在替真设备说话。
所以形态必须①入库 ②落进端侧读的文件 ③界面上常驻显示。

**`host` 的必填性由形态决定。** 真实源没地址 = 合法态（「已登记、未接线」），
模拟源则根本不需要 `host`（用 `sim_host`）。把 `host` 放进"必填键"里，会把
「这台先不接」这种**正常操作**判成错误 —— 老版本就踩过（见 `CHANGES.md` §77）。

**端侧读不到文件 ≠ 出错。** 三处加载器（`14/` 写、`13/` 读、`P3/` 读）共用同一条纪律：

1. 文件**不存在** ⇒ 当"这次不接"，**绝不编一个默认地址**（编了就敢往别人家设备上下发指令）；
2. 文件存在但**格式不对** ⇒ **硬失败**（不降级、不猜）；
3. **未知键** ⇒ **硬失败**（一个拼错的键静默退回 502/2404，是最贵的那种失败）。

**解析只在一处做。** `connconf.target()`（Python 侧）与 `conn_target_host/port()`（C++ 侧）
是全项目唯一把"形态"翻成"实际地址"的地方。别在别处再写一遍 `if kind == 'sim'` ——
写两遍就一定会有一天两边不一致。

### 4.5 四条配置通道② 与「文件不存在 ≠ 没配置」

配置通道② 的口径是「界面填表 → 校验 → 原子落盘 → 端侧进程启动时读」。四条都在
`config/point-map/`（该目录被 gitignore —— 它是运行期产物，不是源码）：

| 端 | ① 接入参数 | ② 表 |
| :--- | :--- | :--- |
| **下端**（站内设备） | `active.conn` | `active.csv`（每台一份） |
| **上端**（调度主站） | `active.upstream` | `active.pointmap.csv`（全站一份，v1.6 起可保存） |

四条**同目录、同口径**（原子写、写 BOM、文件头记 `saved_at` 与 `username`），也共用一条
纪律：**文件不存在 ≠ 没配置**，而是"这次不接、回落默认"。推论：

- **恢复默认的唯一正规途径是 DELETE**（删文件 + 清库存档），**不是保存一个空表** ——
  空表会被 `normalize()` 按 400 拒掉；
- 允许存空表就会出现「界面上什么都没有、端侧按内置默认在跑」，又是一个看不见的第二
  个真相源。

**两条与下端故意相反的地方**（别顺手抄）：

| | 定位方式 | 行序 | 能不能删 / 插 / 换序 |
| :--- | :--- | :--- | :--- |
| 下端 `device_point_map` | **行序即寄存器索引** | 有意义 | **不能** |
| 上端 `active.pointmap.csv` | **靠 IOA 定位** | 无意义 | 可以任意增删 |

### 4.6 模拟输入源：三层模型与「套」（v1.7.x）

设备管理页的模拟源**只改模拟专用列**，这是整块功能唯一要守住的红线：

| 端 | 真实值（一个字都不碰） | 模拟值（启用时才写） |
| :--- | :--- | :--- |
| 下端 | `device_conn.host` / `port` | `device_conn.sim_host` / `sim_port` |
| 上端 | —（`listen_port` / `common_addr` 是**链路属性**） | `upstream_link.master_host` |

上端没有"真实主机地址"这一列：EMS 是**从站**，由调度发起连接，本机只在监听口等。
所以监听口与 CA 在真实态与模拟态下**是同一个值**，模拟主站
（`P3/build/iec104_master.exe`）连的就是这个口。⇒「换一套」时上端那一栏不动是
**正确的**，不是漏做 —— 上端没有第二个可换的量。

**「套」**：`simconf.PRESETS` 四套，`port_base` = 1502 / 1602 / 1702 / 1802；
下端第 k 台的模拟端口 = `port_base + k`。套号存 `meta.sim_preset`（**不是内存**）：
进程重启后要知道上次用到哪一套，否则「换一套」每次重启都从第一套开始，等于没换。
`preset_of(i)` 自带模回绕，转满一圈回到起点。

**为什么"换一套"不能顺手把形态写了**：第一版 `kind = "sim" if want else "real"`，
而 `rotate` 时 `want is None` ⇒ `kind` 落到 `real` ⇒ 点一下「换一套」两端被切回真实源，
而界面上唯一的解释是"套 3"。现在 `kind=None` 表示**沿用两端各自的形态**
（两端不一致就各留各的，不强行拉齐）。守着它的判据是 `14/tests/selftest.py` 的 `[G9]`。

### 4.7 模拟链路：配置之外还得有五个进程（v1.10）

**表不是数据。** v1.7~v1.9 的「启用模拟源」只改配置 —— 表填好了、颜色也变了，
可页面上仍然一行实时值都没有。差的是进程，而界面上当时看不出还差什么。

`src/simproc.py` 把这条链当成**一个可启停的整体**：

| # | 角色 | 程序 | 作用 |
| :--- | :--- | :--- | :--- |
| ① | 实时库初始化器 | `11/build/rtdb_initializer.exe` | **常驻**，建出 RT_DB 共享内存段 |
| ② | 设备替身 ×N | `13/sim/modbus_slave.py --port <该台模拟口>` | 每台启用设备一个 |
| ③ | 采集器 | `07/build/main_field.exe --device modbus` | 写 CSV 实录（→ `step_record`）+ 写 RT_DB |
| ④ | 上端从站网关 | `P3/build/iec104_gateway.exe` | 在 `listen_port` 上等调度 |
| ⑤ | 模拟主站 | `P3/build/iec104_master.exe` | 扮演调度，连 ④ 读上送 |

**①为什么必须常驻**：Windows 上 RT_DB 段是页面文件映射对象，**最后一个句柄关闭
即销毁**。`07/build/ems_rt_db_init.exe` 建完就退 ⇒ 网关起来时看到的是"共享内存
不存在"。所以养着 `rtdb_initializer.exe`。

**③为什么走 `engine.LIVE` 而不自己 Popen**：采集器是唯一入库通道
（`07/` 写 CSV → 尾随线程增量导入 `step_record(scenario='live')`）。自己 Popen
只能得到一份没人读的 CSV。故 `engine.LIVE.start(source="sim", spawn_peer=False)`
—— `spawn_peer=False` 是因为**替身由 simproc 统一起**，同一端口两个替身会互踢。

**边界（要写在脸上）**：`active.conn` 是**单台**文件、`07/ --device modbus` 也是
单目标 ⇒ 哪怕起了 6 个替身，**实际被采集的只有 1 台**（`engine.picked_sim()` 那台）。
面板因此分成两张表：上面是「表上写的端口」，下面是「谁真的在听」。

**「换一套」与「换一个进程」是两件事**：前者改**表**（端口段），后者让**进程**
按当前这一套重新拉起。两者分开是有意的 —— 换套不必打断正在看的采集；但面板
必须提示「进程还在上一套」，否则就是两套并行、都自称正常。

**平台重启后的认领**：`_rows` 是内存里的进程表，后端一退出就空了，而替身 / 采集 /
网关 / 主站是独立进程、照跑。所以每次起停都把它落进 `meta['sim_chain_procs']`，
`server.create_server()` 启动时主动 `CHAIN.attach(conn)` 按 pid + `tasklist` 重建，
`status()` 回 `attached` 标志。**不认领的代价**不是显示错，而是用户再点一次
「换一个进程」⇒ 同一批端口起第二套替身，后者启动即退，看起来像"替身坏了"。
（一个都不在了就清 meta、不 attach —— 不拿全红的表骗人。）

---

## 5. 接口

全部返回 JSON。读接口最低 `viewer`，写接口 `operator`，用户管理 `admin`。

| 方法 | 路径 | 权限 | 说明 |
| :--- | :--- | :--- | :--- |
| GET | `/api/health` | — | 服务与数据库状态、场景清单、各表行数 |
| POST | `/api/login` | — | 登录，返回 token（12 h） |
| POST | `/api/logout` | — | 注销 |
| GET | `/api/me` | — | 当前用户 |
| GET | `/api/overview` | viewer | **首页**：四路功率 + SOC/温度 + 收益 + 告警 + 不变量 + 环状态 |
| GET | `/api/realtime` | viewer | 某一时刻 5 台设备的快照（带品质位） |
| GET | `/api/series` | viewer | 降采样曲线（默认 96 点） |
| GET | `/api/steps` | viewer | 时序明细分页 |
| GET | `/api/commands` | viewer | 指令流水（含 `result` 分布） |
| GET | `/api/alarms` | viewer | 告警列表，`?level=` 与 `?source=` 可单用或组合；**不传 / 传空串 = 不筛选** |
| GET | `/api/alarms/summary` | viewer | 分档统计 + 24 h 时间线，`level` / `source` 两维**交叉筛选**：<br>任一维度的计数带"另一个维度"的条件、**不带自己**，所以筛完还能点回别的取值 |
| GET | `/api/devices` · `/api/devices/{id}` | viewer | 设备台账 |
| POST | `/api/devices` | **operator** | 新增设备台账 |
| GET | `/api/devices/{id}/point-map` | viewer | 点表（库内权威副本）+ 预检统计 + **落盘状态**<br>（`synced` = 盘上点表与库内一致；`conn_synced` = 盘上接入参数与库内一致） |
| PUT | `/api/devices/{id}/point-map` | **operator** | 保存点表：**校验 → 原子落盘 → 写库**，任一步失败整体回滚；<br>落盘含 `active.csv`（点表）与 `active.conn`（接入参数，见 §9） |
| GET | `/api/devices/{id}/point-map.csv` | viewer | 导出点表 CSV（`?template=1` 取内置默认表当模板） |
| PUT | `/api/devices/{id}/conn` | **operator** | 保存接入参数（IP/端口/从站号/轮询/超时/启用/**形态**）：写库 + 原子落盘<br>`config/point-map/active.conn`；**真实源且地址清空则删掉该文件**（不让端侧拿作废地址去连）；<br>返回里带 `target`（端侧实际会连的 `host:port` —— 形态是 `sim` 时是模拟器地址） |
| GET | `/api/access` | viewer | **两端接入总览**（v1.5）：上端链路 + 实际对端 + 落盘状态；下端按 `real`/`sim`/`unset`/`off` **四态**计数 + 逐台明细<br>（`off` = 地址齐了但这一条停用，**不塌进** `unset`） |
| PUT | `/api/access/upstream` | **operator** | 保存上端链路（形态 / 监听网卡 / 监听口 / 公共地址 / 模拟主站地址 / 启用）：<br>写库 + 原子落盘 `config/point-map/active.upstream`；CA 0 与 65535 是保留值、端口越界一律 400 |
| GET | `/api/data-source` | viewer | **两路输入源的状态**（v1.7）：`ends` = 调度主站接入 / 站内设备接入，各带当前形态、对端、实际目标与落盘状态；`mix` 给出两端组合的提示等级与一句人话 |
| POST | `/api/data-source` | **operator** | 切**一端**的输入源：`{end, kind}`，`end` 必填（一次一端，没有「两端一并切」）。下端写穿 `device_conn` 全表 + meta + `active.conn`；上端 upsert `upstream_link` + `active.upstream`。<br>采集中切下端 409 —— 采集器还在按旧参数读写；切上端允许（网关不热更新） |
| GET | `/api/sim-source` | viewer | **模拟输入源总开关的状态**（v1.7.x）：`enabled`（两端**都**在 sim 才算）、两端各自形态、当前套（`preset` / `preset_name` / `port_base` / `preset_count`）、`plan`（下端每台该填的模拟地址与端口）、`note`（"真实值不会被覆盖"那句话）。<br>v1.10 起另带 **`chain`**：`running` / `procs`（角色 / 对象 / 目标 / `alive` / `note`）/ `counts{up,total}` / `preset_matches`（**表与进程是否同一套**） |
| POST | `/api/sim-source` | **operator** | **一个开关管两端**：`{enable: true\|false}` 或 `{rotate: true}`。<br>`enable=true` = 两端切 sim + 按当前套填好模拟专用列 + 重写两份落盘文件 **+ 把整条模拟链路拉起来**；<br>`enable=false` = 两端回 real（真实列从未被动过）**+ 收掉链路**；<br>`rotate` = 套号 +1（四套循环）**且不动形态、不动进程** —— 换的是"哪一组数据"，不是"要不要用模拟源"。<br>采集中 `enable=true` → 409 |
| POST | `/api/sim-source/proc` | **operator** | **模拟链路的进程管理**（v1.10，只管进程、不管表）：`{action: start\|stop\|restart}`。<br>`start` 按**当前这一套**拉起 rtdb 初始化器 / 替身 ×N / 采集器 / 上端网关 / 模拟主站（已在跑 = no-op）；<br>`stop` 逆序收掉（采集器由 `engine.LIVE` 停）；<br>`restart` = 「换一个进程」：全停 → 按当前这一套重拉。<br>非法 action → 400（不静默当成 stop）；进审计（`sim_proc`） |
| GET | `/api/upstream/point-map` | viewer | **上端第二份输入：IOA 转发表**（v1.6 起**可保存**）。三态：有平台那份则 `source="platform"`（读 `active.pointmap.csv`，并从文件头回读 `saved_at` / `username`）；没有则 `source="default"`（回落 P3/ 随包底本）。逐行给 IOA / 方向 / 名称 / 类型 / EMS 点名 / 系数 / 单位 / 说明，另给 `counts`、`dup_ioa`、`bad_lines`、`stored`（库存档行数） |
| PUT | `/api/upstream/point-map` | **operator** | **保存转发表**：`{rows}` 或 `{csv}` → 过 `upmapconf.normalize()` → 原子落盘 `config/point-map/active.pointmap.csv` → 整表重写 `upstream_point_map`。任一步失败**文件与库一起退回**（半成功 = 界面说配好了、网关还在用旧表） |
| DELETE | `/api/upstream/point-map` | **operator** | **恢复默认**：删掉文件 + 清库存档，如实报 `removed`。<br>★ 这是恢复默认的**唯一正规途径** —— 空表会被 400 拒（「文件不存在 ≠ 没配置」，见 §4.5） |
| GET | `/api/upstream/point-map.csv` | viewer | 导出当前生效的转发表；`?template=1` 原样输出 P3/ 随包底本当模板 |
| GET | `/api/access-draft` | viewer | **「接入调度主站」九步向导的 schema + 草稿 + 进度**（v1.14）：<br>`steps`（**九步字段清单的唯一一份**，含 `req` / `dep` / `hint`）、`draft`（`steps` 内容 + `current_step` + `completed_at` + `updated_by/at`）、<br>`progress`（总 `required/filled` + 逐段 `required/filled/ok` + `ready`）、`missing`（逐条给**第几步 / 步名 / 字段 / 标签**，前端直接拿来标红与跳页）。<br>★ 必需条数是**动态**的：`dep` 条件不成立的那几条不算，所以空表口径是 38 而不是 41 |
| PUT | `/api/access-draft` | **operator** | 保存草稿（v1.14）：`{steps, current_step, completed?}`。<br>★ **缺项也照存**（"填了就存"，防抖 500 ms 一次）—— 拦必填会逼人填假日期；<br>`completed: true` 才校验，缺项回 **400** 且文案指明第几步缺什么，**不落库**；<br>校验用的是**本次提交上来的内容**，不是库里旧草稿（否则"刚填完就点完成"会被上一次没到的保存误拒） |
| DELETE | `/api/access-draft` | **operator** | **重置**（v1.14）：只清 `meta.access_draft`。<br>★ **不动** `active.upstream`（链路参数）与 `active.pointmap.csv`（转发表）—— 混着清会把线上正在用的配置一起抹掉，返回的 `note` 里把这条写明 |
| GET | `/api/access-draft.csv` | viewer | 导出（带 BOM，Excel 直接双击不乱码）。<br>`?step=N` 导出**那一步自己的**那份：第 2 步 = 《远动信息参数表》（逐点 + CT/PT 变比与遥测满度空列）、第 3 步 = 号段对照表、第 8 步 = 传动记录（下行点逐行）、第 9 步 = 投运清单 + 九步汇总；不带 step 仍是九步总台账。<br>★ 列与名字都由 `accessconf.DOCS` / `export_step` 决定，前端不拼 |
| GET | `/api/strategies` | viewer | 9 个策略与优先级 |
| PUT | `/api/strategies/{id}` | **operator** | 改启用/优先级/参数（进审计） |
| GET | `/api/economics` · `/api/energy` | viewer | 经济性 / 能量统计 |
| GET | `/api/report?period=day\|week\|month` | viewer | 报表聚合 |
| GET | `/api/invariants` | viewer | 安全红线计数 |
| POST | `/api/control/command` | **operator** | 下发指令（**见下**） |
| GET | `/api/audit` | admin | 操作审计 |
| GET · POST | `/api/users` | admin | 用户列表 / 新建 |
| GET | `/api/export/{timeseries\|alarms\|commands\|devices}` | viewer | 导出 CSV |
| GET | `/api/engine` | viewer | **引擎状态**：`run` / `sim` 两种模式、当前数据集、数据源清单（模型源 / 现场设备源 + 各自可用性） |
| POST | `/api/engine` | **operator** | 切引擎模式（运行 ↔ 仿真）；切模式会**重解析默认数据集**，运行模式默认落到 `live` |
| POST | `/api/engine/live/{start\|stop\|reset}` | **operator** | 实时引擎启停与重置：`start` 拉起实时源并开始尾随入库（返回 pid 与实录文件路径），<br>`stop` 有序停掉，`reset` 清空该场景数据（**先停再清**） |
| GET | `/api/sim/runs` | viewer | 仿真台账列表 + `kinds` 元数据（日 / 周 / 月的时长、步长、`log_every`） |
| POST | `/api/sim/runs` | **operator** | 发起仿真（`kind` = `day`/`week`/`month`，可选 `fault`），返回 `run_id` |
| GET · DELETE | `/api/sim/runs/{run_id}` | viewer / **operator** | 单次仿真进度与结果摘要 / 删除（**连带清 6 张表** + 产物目录） |

### 关于"下发指令"

现场**没有真实执行器**时，`POST /api/control/command` **不假装成功**：

```json
{ "accepted": false, "executor": "none",
  "reason": "现场尚未接入真实执行器（无真机）；本次只记录意图并审计",
  "verdict": "out_of_permission", "permission_range": {"lower_kw": -143.3, "upper_kw": 250.0} }
```

它会拿本次指令与本拍的**权限区间**（`p_lower` / `p_upper`）对照，给出
`ok` / `out_of_permission` / `gated` 的判断，并把意图写进审计。
**绝不会在界面上显示一个没有发生的动作。**

---

## 6. 用户与权限

| 角色 | 能做什么 | 现场对应 |
| :--- | :--- | :--- |
| `viewer` | 全部只读 | 运行人员 |
| `operator` | 只读 + 改运行参数 + 下发指令 | 值班长 |
| `admin` | 全部，含用户管理与审计 | 系统管理员 |

口令：`pbkdf2_hmac(sha256)` + 每用户随机盐（12 万次迭代），不存明文、不存裸哈希。
会话：随机 token 落 `session` 表，12 h 过期，可撤销。

> **⚠ 现场部署前必须改掉三个默认口令**（`admin/admin123`、`operator/operator123`、
> `viewer/viewer123`）。首次启动时服务会把这句提示打在控制台上。

### 6.1 ★ 上现场前必做的三条（风险是**相乘**，不是相加）

单独看每一条都"还好"，但它们叠在一起会互相放大：

| # | 项 | 现状 | 上现场前要做什么 |
| :--- | :--- | :--- | :--- |
| 1 | **明文 HTTP** | `server.py` 是裸 `ThreadingHTTPServer`，**没有 TLS**；口令与 token 在链路上是明文 | `16/src/tls.h` 已做出 Windows SChannel 客户端，但**这与"真的开启 TLS"是两件事**；开启步骤在 `19/docs/现场作业指导书.md` |
| 2 | **默认口令在库里** | 由导入器种入 `app_user` 表 | 改掉上面三个口令（同上） |
| 3 | **跨域** | `2026-10-11` 起**默认关闭**（见下） | 保持默认；确需外部页面调测时才 `--allow-origin` |

**★ 关于跨域（2026-10-11 收紧）**：原先 `_send()` 无条件回
`Access-Control-Allow-Origin: *`，等于**允许任意第三方网页直接调本项目的 `/api/*`** ——
只要用户在同一个浏览器里登录过本平台，任一站点都能借他的会话读数据，用户完全察觉不到。
全仓库此前**没有任何文档讨论过它**，是个未被论证过的默认值。

现在的行为：

- **默认**：不回 `Access-Control-Allow-Origin`。托管模式下（`15/` 由本进程 `--static`
  发布）前端与 API **同源**，浏览器本来就不依赖 CORS 头 —— `15/tests/e2e_probe.html`
  用的也是相对路径 `w.fetch('/api/health')`，不受影响。
- **需要时**：`python 14/src/server.py --static 15 --allow-origin http://127.0.0.1:5173`，
  回显的正是你指定的那个源（不是 `*`），**并打在启动横幅上** —— 开关状态必须看得见。

> 为什么用显式开关而不是直接删掉：跨域调试是真实需求，删了会把人逼回给它加 `*`。
> 但**`*` 与"指定某个源"差别巨大** —— 前者是"谁都行"，后者是"就这一个"。
> 让它默认关、且开着的时候看得见，比悄悄留一个 `*` 安全得多。

**这条收紧现在有断言守着（自检 [L] 段）**：AST 静态扫描 + 真起服务发请求的端到端判据。
收紧本身不是成果，**"改回去会变红"才是** —— 上一版收紧完没有任何判据覆盖，
谁为了"页面调不通"顺手改回 `*`，全仓库会一片绿。详见 §7 的 L 段与 `CHANGES.md` §62。

---

## 7. 自检

```bat
14\scripts\run_selftest.bat
```

十七个段、**1118 断言**（2026-10-20 实测；`[K]` 段按库里 distinct `level` × `source` 组合出断言，故此数随告警数据而变）：

| 段 | 内容 |
| :--- | :--- |
| A schema | 20 张业务表齐全（v1.1 加 `run_mode`，v1.2 加设备接入两张 + `scenario.time_base`，v1.4 加 `sim_run` 仿真台账，v1.5 加上端链路 `upstream_link`，v1.6 加上端转发表存档 `upstream_point_map`） |
| B 导入 | 行数（两场景各 8640 拍 / 17280 条指令 / **296** 条告警：normal 7 + fault 289） |
| **C 数据一致性** | **入库值逐项对照 `summary.json`** —— 证明平台展示的不是编造的数字；<br>含**电量交叉验证**（关口电量 = `e_import − e_export`，见 §7.1）。<br>★ 2026-10-11 改：原先断言「fault 场景不变量**不过**」(`iv_f["all_ok"] == 0`)，<br>那是条**负向不变量** —— 只有安全层真失效才变绿。现改为「与 summary.json 逐项相符」<br>+「fault 告警 > normal 告警（证明注入确实生效）」，详见 `CHANGES.md` §60.6 |
| D 幂等 | 重复导入不产生重复行、不漂移；含**跨日重导**（见 §7.2） |
| E 认证与权限 | 口令哈希、token 生命周期、三角色边界 |
| F REST 接口 | **30 个端点路径**的状态码与关键字段（含 401/403/404/400 负路径）；<br>引擎与仿真相关接口（`/api/engine*`、`/api/sim/runs*`）另在 **[J] 段**覆盖；接入向导的 4 条（`GET/PUT/DELETE /api/access-draft` + `GET /api/access-draft.csv`）在 **[R] 段**覆盖（路由表共 **56** 条） |
| G 设备接入闭环 | 点表校验镜像 13/ `validate_map()`、落盘文件判据、保存回滚、<br>**配置通道②**（接入参数落盘 `active.conn`：内容/幂等/清空即删/回滚/解析器严格性）、<br>**两条跨语言判据**（13/ 的 C++ 进程分别读平台写出的 `active.csv` 与 `active.conn`）；<br>**【G6】两端接入**（v1.5）：下端切模拟源后 `target.port` 必须变、文件头必须写"不是真机"，<br>上端改监听口 / CA 必须落盘且 P3 能读，CA 越界与 viewer 写一律拒；<br>**【G8】上端转发表可保存**（v1.6）：PUT / DELETE 成对、先过 `normalize` 再落盘、落盘→写库且失败回滚、<br>24 上送 + 6 下行、8 类坏表 400 且文件一字未动、空表 400、viewer 403、CSV 导出与模板；<br>**【G9】模拟输入源总开关**（v1.7）：两端一起切/回、`rotate` 不动形态、真实值逐字段未变、<br>`off`（停用）与 `unset`（未配置）不塌成一个状态；<br>**【G10】模拟链路进程**（v1.10）：`chain` 结构齐全、每台设备一行替身（停用的那台也要说明为什么不起）、<br>**替身端口与表逐台一致**、采集器行写明采哪一台、非法 action 400、viewer 403、操作进审计；<br>★ 自检**不真拉进程**（`EMS_SIMPROC_OFF=1`）—— 真起 9 个进程会把实录导进临时库，反而把 `[I]` 的三条断言弄红 |
| H 旧库迁移 | v1.1 → v1.2 → v1.3 → v1.4 → v1.5 的幂等 `ALTER TABLE`（且**不产生多余的 ALTER**）；老库补 `source_kind` 一律填 `'real'` —— 老库里没有任何"这是模拟"的痕迹 |
| I 实时入库 | 07/ `--record` 的 wall 口径、增量幂等、台账兜底、`h_series` 时间轴分支 |
| **J 引擎双模式** | **运行 / 仿真两条数据链路**：`engine` 切换与权限、仿真调度端到端入库、<br>**实时引擎真的被拉起 / 写实录 / 增量入库 / 被停掉**、`time_base=wall` 口径、<br>仿真产物删除连带清 6 张表、跨日能量分摊不变量 |
| **K 告警分类** | 等级 × 来源**交叉筛选**端到端：以**独立 SQL 为标尺**做恒等式校验，<br>覆盖全部 level × source 组合 + 空串语义（= 不筛选）；<br>判据不写死条数（数据集会变），改校验三条交叉求和恒等式 |
| **L 跨域(CORS)** | 守 §61 的收紧成果，两层互补缺一不可：<br>**静态层** —— AST 扫 `14/src/*.py`，不许出现字面量 `"*"` 作<br>`send_header("Access-Control-Allow-Origin", …)` 实参或 `*.allow_origin` 赋值；<br>**端到端层** —— 真起服务发 HTTP 请求，断言默认响应头**不带** ACAO、<br>白名单模式回显**指定源**（且只此一个）。<br>★ 为什么要端到端：源码里一个 `*` 都没有，洞也可能在（例如给默认兜个非空值），<br>静态层证明不了这条 —— 反证见 `CHANGES.md` §62 |

| **M 静态目录穿越** | 2026-10-19 体检补：静态目录的越权判据原来只 `startswith(root)`，<br>`15` 与 `15_backup` 这种**前缀相同的兄弟目录**能过。判据真起服务发 `GET /../<兄弟>/x`，<br>断言 **403 且 body 一个字节都不回**，同时确认同目录正常文件仍 200（收紧没把静态服务弄坏） |
| **N 链路启动耗时** | 2026-10-19 补：点一次「启用模拟源」原来 HTTP 要 **25.4 s** 才返回<br>（5 台替身**各**盲等 2.5 s + 采集器死睡 3.6 s + 主站 3.0 s），用户看到的就是"点了没反应"。<br>这一段守的是「等待窗口**整组共用**、且靠**证据**提前返回」（端口连得上 / CSV 写出第一拍就走），<br>而不是"把 2.5 调成 1.2"—— 后者多加几台设备就又慢回去。详见 `CHANGES.md` §92 |
| **O 链路操作走后台** | 2026-10-19 补：就算压到 6.9 s，用户点一下仍要**盯着一个转圈的请求**等这么久。
  `/api/sim-source`（给了 `enable`）与 `/api/sim-source/proc` 现在返回 **202「收下了」**，
  进程操作交给后台线程，前端轮询 `chain.job`（`running` 期间是"在办"，`ok` 才是结果）。
  ★ 契约变了：**返回成功 ≠ 链路已经起来**。已经有一个在跑 ⇒ **409 不排队**
  （排两次让用户等两倍时间，后一次还会把前一次盖掉）。自检模式（`_no_spawn()`）下仍
  **同步跑完** —— 异步会让"操作有没有进审计"变成竞态，断言不该摇。详见 `CHANGES.md` §92b |
| **P 报错不许被弄丢** | 2026-10-19 全站体检补。共性病灶：**界面最后那句「发生了什么」
  自己失败了，也不留痕迹** ⇒ 用户看到「数据不动了，但哪儿都没说坏了」。
  三处：`engine` 把 `live_state/live_error` 写进库的兜底 `except Exception: pass`（
  §91 只管了"别把可恢复错误判死"，没管"连原因都写不进去"这层）；`api._file_restore`
  回滚失败却静默，而它的 7 处调用方会告诉用户「已整体回滚」—— 那句话会变成假话；
  以及 `ConnectionResetError` 刷满日志（实测 1228 行里 52 个 Traceback，**100% 是它**）。
  新增 `engine.note_critical()`（stderr + `14/data/critical.log`，自身永不抛异常）与
  `ApiServer` 覆盖 `handle_error`。★ 要覆盖的是 **Server** 侧（继承自 `BaseServer`），
  不是 Handler 侧。详见 `CHANGES.md` §93 |
| **Q 时刻标签不折回** | 2026-10-19 用户报「实时检控永远是 0:00」。根因两层：
  一天最后一拍 `t_s = 86400` 被 `%24` 折成 `00:00`（与当天第一拍同标签）；
  而 `realtime/overview` 直接贴 `row["time_str"]`（那是 10/ 写死的 HH:MM，同样折回）。
  现在 `_hhmm` 的边界为：`86400 → 24:00`、`86401 → D2 00:00`、`604800 → D7 24:00`；
  两个接口的时刻改由 `_time_label()` **按 t_s 现算**，与曲线横轴同源。
  这一段判据是"真跑函数算出什么"（8 个边界值），不是"源码里有没有某个字"——
  注释里为了说明问题本身就得写出那个旧式子，全文件扫描会被自己的注释命中。详见 `CHANGES.md` §94 |
| **R 接入调度主站九步向导** | 2026-10-20 补。`accessconf.STEPS` 是九步字段清单的**唯一一份**
  （56 字段 / 41 条 `req`，其中 3 条条件必填；空表口径适用 38 条），
  界面渲染、`completed` 校验、CSV 导出的列全从它来 —— 前端写死一份就会出现
  「界面有、导出件没有」而两边都显示成功。这一段守的是：
  步骤号连续且**只有第 6 步** `config`、不预填「通过」这类结论、
  `req` 与 `dep` 同时出现时 `req` 必须为真（写成 `req: False` + `dep` 会让条件必填**永不生效**）、
  `PUT` 缺项也照存而 `completed=true` 才校验、被拒时**不半路落库**且文案指出第几步缺什么、
  `DELETE` 只清草稿（`active.upstream` 与 `active.pointmap.csv` 一字不动）、
  CSV 带 BOM 且行数随填充分变化。★ 草稿存 `meta.access_draft`（不是 localStorage）：
  换机器/换浏览器仍在，且能记「谁最后填的、什么时候」。**v1.15 追加**：`accessconf.DOCS` 给九步各写了一份材料清单（含依据与「现状」真数）、可一键采用的建议值、以及本步自己的导出件；这几样全在后端，前端只负责画 —— 文案写死在前端就会出现「界面说 10 项、导出件少两行」。详见 `CHANGES.md` §96 / §97 |

`10/build` 不存在时整个自检显式打印 `SKIPPED=1` 并以 0 退出 ——
环境缺失不是代码缺陷，但**必须显式出现**（沿用 `13/` 定下的规矩）。

### 7.1 电量曾被放大 8640 倍（2026-09-25 修复，护栏已加）

`energy_statistics` 的 `pv_kwh` / `load_kwh` / `grid_kwh` 原本按

```
Σ(功率)  ×  (总时长 / 3600)          ← 错的
```

计算：`Σ(功率)` 是 8640 个 kW 相加，再乘 24 h，等于**把每一拍的功率都当成
持续了 24 小时** —— 结果放大了 8640 倍。实测光伏 2291 kWh 被写成 `19794361`，
关口 5180.8 kWh 被写成 `44762095`。

修法是逐拍用**该拍自己的 `dt`** 加权，再统一换算 kWh：

```sql
SUM(p_pv_kw * dt) / 3600        -- dt = t_s - LAG(t_s, 1, 0) OVER (ORDER BY t_s)
```

**为什么没被测试拦住**：当时的 C 段只逐项核对 `economics` 的 12 个字段，
**没有一条断言碰过 `energy_statistics`**。而同一张表里 `charge_kwh` / `discharge_kwh`
是直接取自 `10/` 的经济性核算（本来就是对的值）—— 于是「536.6 与 19794361 并排」
这种一眼可见的异常，在表格里沉了很久，最后是**在浏览器里看表格**才发现的。

**补的三条护栏**（C 段，逐场景）：

| 断言 | 输入错误值时的表现 |
| :--- | :--- |
| 关口电量 = `e_import_kwh − e_export_kwh`（±5 kWh） | `44762095.368 ≠ 5180.7988` ❌ |
| 五个电量同数量级（最大/最小 < 100） | 比值 `122958.5` ❌ |
| 光伏 ∈ (1000, 6000)、负荷 ∈ (3000, 12000) kWh/日 | `19794361` / `65821289` ❌ |

**护栏有效性已反向验证**：把公式临时改回放大 8640 倍，**8 条断言全部命中**
（normal / fault 各 4 条），且报错信息直接印出正确期望值 `5180.7988±5.0`，
不需要人工再算一遍。改回正确公式后 `PASS=209 FAIL=0`
（后又补 3 条跨日护栏，见 §7.2，共 **212**）。

> 教训：**同表内量级不一致就是单位错**。这类错误不需要领域知识就能发现，
> 但前提是有条断言真的去看过那个字段 —— 而不是假设"导入器写的应该没错"。

### 7.2 跨日重导会把旧行堆积下来（2026-09-25 修复，护栏已加）

修完电量、重导数据库时发现 `energy_statistics` 里 `2026-09-25` 与 `2026-09-26`
**两天的行并存** —— 前者还是修复前的 `pv_kwh = 19794361`，后者是修好的 `2291.014`。

根因：这张表的主键是 `(date, scenario_id)`，而 `date` 取**导入当天**，
不是数据本身的运行日。`aggregate_energy()` 当时只做 `INSERT OR REPLACE`：

- **同一天重导** → 主键完全重合，覆盖得干干净净，看不出问题；
- **跨天重导** → 主键不同，直接**新增一行**，旧数据原地留着。

它也是唯一漏了"先清后写"的表（其余表都在写入前按 `scenario_id` 清过）。修法是补一行
`DELETE FROM energy_statistics WHERE scenario_id = ?` 再写。

**为什么自检抓不到**：自检用 `tempfile` 建**全新库**，D 段"重导两次"用的又是
**同一个 day** —— 主键完全重合，这条路径**测试根本走不到**。补的 3 条断言专门制造跨日：

| 断言 | 它防的是什么 |
| :--- | :--- |
| `date="2099-01-02"` 重导后行数不变 | 跨日导入产生**新行** |
| 重导后 `COUNT(DISTINCT date) == 1` | 表里应只剩**一个运行日** |
| 恢复默认导入后行数仍不变 | 反复导入的**幂等性** |

> 教训：`INSERT OR REPLACE` 的"幂等"只在**主键取值为业务常量**时成立。
> 主键一旦掺进"当前时间"这类**随执行环境变化**的量，它就退化成"每次执行都新增一行"。
> 与 §7.1 同一类：**测试走过的路径没问题，没走过的路径照样是雷。**

### 7.3 实时入库（07/ --record → live 场景，time_base='wall'）

07/ 现场进程 `--control --record 实录.csv` 边跑边把每拍追加写成 20 列 CSV（与
`10/ timeseries.csv` 同构，但 `t_s` 是 Unix 墙钟秒、`time` 是完整日期时间）。
14/ 侧增量入库：

```bat
14\scripts\import_live.bat 实录.csv live
```

与 `import_scenario` 的三点区别（对应自检 `[I]` 段）：

1. **`time_base='wall'`**：墙钟秒与仿真"一天内秒"是两套时间轴，不可混算。
   曲线接口 `h_series` 按口径分支 —— `wall` 场景的 `time` 直接用 `time_str`
   （完整日期时间），而不是 `_hhmm(t_s)`（那会把墙钟秒 `%86400` 折成"一天内第几秒"）。
2. **增量追加**：不 DELETE 旧行，主键 `(scenario_id, t_s)` 天然幂等，可反复调用。
3. **台账兜底**：`control_command` 有外键到 `device`，首次 live 导入时若台账为空，
   自动用默认额定值补最小台账。

> 缺口（诚实记录）：live 阶段只进 `step_record` 与 `control_command`，
> 经济性/不变量要等收尾才能算；`EmsRuntime` 内部 `t_` 仍是理想时间轴
> （见 07/docs §9.5），长时间在线会漂移 —— 这是 #106 留下的尾巴。

---

## 8. 已知边界

| 边界 | 说明 |
| :--- | :--- |
| 无真机实时源 | 已打通：07/ 现场进程 `--record` 写 CSV → `scripts\import_live.bat` 增量入库（`scenario.time_base='wall'`）。但 `EmsRuntime` 内部 `t_` 仍是理想时间轴（见 07/docs §9.5），长时间在线会漂移 |
| 无 HTTPS | 只监听 `127.0.0.1`。对公网开放前必须加 TLS 与反向代理 |
| 无界面 | 本模块只到接口层；界面在 `15/`（9 页，已就绪） |
| 会话在内存表 | `session` 落 SQLite，多实例部署需换成共享存储 |
| 无历史数据保留策略 | 时序全量入库。长期运行需要按保留期归档（与 P5 部署一起做） |
| 未接 P2 事件流 | `soe.csv` / `metrics.json` 尚未定时导入，当前告警来自 `10/` |

---

## 9. 涉及文件

- 本模块：`schema.sql` · `src/{emsdb,importer,auth,api,server}.py` ·
  `src/{pointmap,connconf}.py` · `tests/selftest.py` · `scripts/*.bat`
- 下发给端侧的配置（两条通道，同目录、同一次保存）：
  `config/point-map/active.csv`（点表，`pointmap.py` 写）·
  `config/point-map/active.conn`（下端接入参数，`connconf.py` 写）·
  `config/point-map/active.upstream`（上端链路，`uplinkconf.py` 写）
  → 13/ 07/ 启动时读（`EMS_POINT_MAP_DIR` 可整体搬走该目录）
- 数据来源：`10/build/{timeseries.csv,alarms.csv,summary.json,fault/}`（真实产物）
- 口径依据：`docs/architecture.md` §3.1/§22/§23 · `04/src/strategies_9.h` ·
  `07/src/rtdb/ems_point_table.h` · `docs/交接/下一步工作交接说明.md` §3.2
- 界面：`15/`（下一步）
