# 14/ — 平台层后端 + SQLite 数据库

> **一句话**：把 EMS 的数据装进数据库、对上开 REST 接口，让
> 「这个站现在怎么了、今天赚了多少、有没有越限」不用再去翻 8 万行时序 CSV。

对应设计方案 [`docs/architecture.md`](../../docs/architecture.md) 的
**§3.1 第四层「平台层」**、**§22 系统数据库设计**、**§24 E（后端）/ F（前端）** 两岗。

---

## 1. 为什么需要它

`01`–`13` / `P1`–`P3` 已经把**设备层**与 **EMS 核心控制层**做透了（当时 9748 断言 / 33 组件；
补 `16/`~`20/` 后为 **16473 断言 / 41 步**（2026-10-08 四项界面补漏后；`17/` 四个选配模拟器落地时是 16276），
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
| `src/api.py` | REST 路由与业务逻辑（41 个端点，含引擎与仿真调度） |
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
| PUT | `/api/devices/{id}/conn` | **operator** | 保存接入参数（IP/端口/从站号/轮询/超时/启用）：写库 + 原子落盘<br>`config/point-map/active.conn`；**地址清空则删掉该文件**（不让端侧拿作废地址去连） |
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

十二个段、**664 断言**（2026-10-17 实测；`[K]` 段按库里 distinct `level` × `source` 组合出断言，故此数随告警数据而变）：

| 段 | 内容 |
| :--- | :--- |
| A schema | 18 张业务表齐全（v1.1 加 `run_mode`，v1.2 加设备接入两张 + `scenario.time_base`，v1.4 加 `sim_run` 仿真台账） |
| B 导入 | 行数（两场景各 8640 拍 / 17280 条指令 / **296** 条告警：normal 7 + fault 289） |
| **C 数据一致性** | **入库值逐项对照 `summary.json`** —— 证明平台展示的不是编造的数字；<br>含**电量交叉验证**（关口电量 = `e_import − e_export`，见 §7.1）。<br>★ 2026-10-11 改：原先断言「fault 场景不变量**不过**」(`iv_f["all_ok"] == 0`)，<br>那是条**负向不变量** —— 只有安全层真失效才变绿。现改为「与 summary.json 逐项相符」<br>+「fault 告警 > normal 告警（证明注入确实生效）」，详见 `CHANGES.md` §60.6 |
| D 幂等 | 重复导入不产生重复行、不漂移；含**跨日重导**（见 §7.2） |
| E 认证与权限 | 口令哈希、token 生命周期、三角色边界 |
| F REST 接口 | **29 个端点路径**的状态码与关键字段（含 401/403/404/400 负路径）；<br>引擎与仿真相关接口（`/api/engine*`、`/api/sim/runs*`）另在 **[J] 段**覆盖（路由表共 41 条） |
| G 设备接入闭环 | 点表校验镜像 13/ `validate_map()`、落盘文件判据、保存回滚、<br>**配置通道②**（接入参数落盘 `active.conn`：内容/幂等/清空即删/回滚/解析器严格性）、<br>**两条跨语言判据**（13/ 的 C++ 进程分别读平台写出的 `active.csv` 与 `active.conn`） |
| H 旧库迁移 | v1.1 → v1.2 → v1.3 → v1.4 的幂等 `ALTER TABLE`（且**不产生多余的 ALTER**） |
| I 实时入库 | 07/ `--record` 的 wall 口径、增量幂等、台账兜底、`h_series` 时间轴分支 |
| **J 引擎双模式** | **运行 / 仿真两条数据链路**：`engine` 切换与权限、仿真调度端到端入库、<br>**实时引擎真的被拉起 / 写实录 / 增量入库 / 被停掉**、`time_base=wall` 口径、<br>仿真产物删除连带清 6 张表、跨日能量分摊不变量 |
| **K 告警分类** | 等级 × 来源**交叉筛选**端到端：以**独立 SQL 为标尺**做恒等式校验，<br>覆盖全部 level × source 组合 + 空串语义（= 不筛选）；<br>判据不写死条数（数据集会变），改校验三条交叉求和恒等式 |
| **L 跨域(CORS)** | 守 §61 的收紧成果，两层互补缺一不可：<br>**静态层** —— AST 扫 `14/src/*.py`，不许出现字面量 `"*"` 作<br>`send_header("Access-Control-Allow-Origin", …)` 实参或 `*.allow_origin` 赋值；<br>**端到端层** —— 真起服务发 HTTP 请求，断言默认响应头**不带** ACAO、<br>白名单模式回显**指定源**（且只此一个）。<br>★ 为什么要端到端：源码里一个 `*` 都没有，洞也可能在（例如给默认兜个非空值），<br>静态层证明不了这条 —— 反证见 `CHANGES.md` §62 |

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
  `config/point-map/active.conn`（接入参数，`connconf.py` 写）
  → 13/ 07/ 启动时读（`EMS_POINT_MAP_DIR` 可整体搬走该目录）
- 数据来源：`10/build/{timeseries.csv,alarms.csv,summary.json,fault/}`（真实产物）
- 口径依据：`docs/architecture.md` §3.1/§22/§23 · `04/src/strategies_9.h` ·
  `07/src/rtdb/ems_point_table.h` · `docs/交接/下一步工作交接说明.md` §3.2
- 界面：`15/`（下一步）
