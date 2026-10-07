# 19/ — 现场部署（P5）

> 把 EMS 从"能在开发机上手动跑 exe"推进到"能装到站上服务器、有人守着、
> 出事能查、配置能回滚"。本模块**只做编排与运维**，不碰控制律、不碰点表语义、
> 不碰通信协议 —— 那些是 `05`~`11` / `P1`~`P3` 的事。
>
> **本模块零第三方依赖**（约定 §1）：只用 Python 3 标准库 + 系统自带的
> `cmd.exe` / `schtasks.exe`。不引 pywin32、不引 pytest、不引任何 pip 包。

**当前状态（阶段成果）**：4 个测试套件，**PASS=424 FAIL=0 SKIPPED=0**，
`19/scripts/build_test.bat` 退出码 0。断言分布：
`test_config_store` 86 · `test_log_rotator` 84 · `test_supervisor` 73 ·
`test_scripts` 181。

> 口径说明（约定 §5.2）：上面的 424 是**前置齐备口径**——`11/build/*.exe`、
> `P1/build/ems_config.exe`、`13/sim/modbus_slave.py` 都在。
> 缺其中任何一项时，对应用例会打印 `SKIPPED=n`（**不是 FAIL**）并以 0 退出，
> 所以 PASS 会相应减少。唯一真相源是根 `README.md` 的「四口径表」。

---

## 1. 它在整个工程里的位置

`19/` 处在**最外层**：它不 include 任何人，只"编排"别人已经做好的东西。

```
                       ┌────────────────────────────────────────────┐
                       │  11/ 三进程（现场就是这三个 exe）           │
                       │   rtdb_initializer.exe   常驻保活 RT_DB 段   │
                       │   device_side.exe        设备侧              │
                       │   ems_side.exe           EMS 侧              │
                       └───────────────────┬────────────────────────┘
                                           │  按 DAG 顺序拉起 + 存活检测
                                           │  + 异常重启 + 退避 + 级联
   ┌───────────────────────────────────────▼────────────────────────┐
   │  19/src/ems_supervisor.py      进程守护器（P5 之一）             │
   └───────┬───────────────────────────┬────────────────────────────┘
           │                           │
   ┌───────▼──────────────┐   ┌────────▼─────────────────────────┐
   │ P1/ ems_config.exe   │   │ P2/ ems_side 导出的 SOE / metrics │
   │ 89 字段配置绑定表     │   │  SoeLog（有界 4096 条）           │
   └───────┬──────────────┘   └────────┬─────────────────────────┘
           │ --capture / --dump-template        │ to_csv() / to_prometheus()
   ┌───────▼───────────────────────────────▼────────────────────────┐
   │  19/src/config_store.py      配置落盘与版本（P5 之二）            │
   │  19/src/log_rotator.py       SOE/metrics 落盘与轮转（P5 之三）    │
   └───────┬───────────────────────────────┬────────────────────────┘
           │                               │
   <安装根>/conf/ems_config.json   <安装根>/log/soe/*.csv
   <安装根>/conf/history/vNNNN_*   <安装根>/log/metrics/*.prom

   点表核对（文档侧）： 19/docs/现场作业指导书.md  ⇄  13/sim/modbus_slave.py --dump-tsv
                        13/build/modbus_probe.exe（读真设备一遍）
```

### 与相邻模块的边界

| 模块 | 关系 |
| --- | --- |
| `11/` | **被守护者**。19/ 只读它的 `docs/README.md` 与三进程的**日志行前缀**（`[INITIALIZER] segment` / `[DEVICE] connected` / `[EMS] connected`）作就绪判据，**不改 11/ 任何文件** |
| `P1/` | 配置的**唯一语义源**。89 字段的校验、模板、`--capture` 全在 P1；19/ 的 `config_store` 只负责"落盘 + 版本 + 回滚"，**不重复实现字段语义** |
| `P2/` | SOE/metrics 的**生产者**。19/ 的 `log_rotator` 刻意沿用 P2 `SoeLog::csv_header()` 的 9 列契约（`t_first,t_last,duration_s,repeat,level,source,code,message,data`），使导出文件能被原样接手 |
| `13/` | **点表对账的参照**。指导书里那张 32 点表与 `13/sim/modbus_slave.py --dump-tsv` 逐点对账（测试 [D] 段）；`13/build/modbus_probe.exe` 是现场读真设备的工具。19/ 不依赖 13/ 的代码 |
| `07/` | 只读引用 RT_DB 的**部署常识**（段是页文件映射对象、最后句柄关就销毁），据此得出"初始化器必须常驻"。不改 07/ |
| `scripts/build_all.bat` | **不改**（约定 §6 由集成方统一执行；本次施工硬性禁止） |

---

## 2. 设计决策与"为什么"

### 2.1 部署形态：**计划任务**，不是 Windows 服务

这是任务书要求"做出决定并给出理由"的那一条。结论：用
`schtasks /SC ONSTART /RU SYSTEM /RL HIGHEST`，由
`19/scripts/install_task.bat` 注册。

1. **零第三方依赖**。`schtasks.exe` 系统自带；真·Windows 服务要做 SCM 握手
   （`StartServiceCtrlDispatcher` + 周期报 `SERVICE_RUNNING`），Python 侧要握手就得引
   pywin32 —— 直接违反约定 §1。
2. **不握手会被 SCM 杀掉，且现象极具误导性**。把控制台程序用 `sc.exe` 注册成服务，
   SCM 等约 30 秒收不到 `SERVICE_RUNNING` 就把进程杀掉；`sc query` 只显示
   `STOPPED` —— 现场看到的是"服务自己崩了"，真因是**根本没握手**。
   所以 `install_service.bat` **故意拒绝安装**（退出码 **2**，不是 1），只打印配方。
3. **服务化真正值钱的部分，计划任务一样有**：开机自启、SYSTEM 账号、无控制台窗口、
   `schtasks /Run|/End|/Query` 管理。缺的只有 `services.msc` 里的可见性与
   系统级失败恢复策略。
4. **顺序与退避无论如何都得自己写**。这三个进程有硬依赖顺序（见 2.2），
   外面包服务还是包任务都一样 —— 那部分由 `ems_supervisor.py` 自己做。

**什么时候该换服务路线**（满足任一条）：

- 客户运维规范要求进程出现在 `services.msc`，且必须能用 `net start/stop` 管；
- 需要"服务失败自动恢复"的**系统级**策略与事件日志集成；
- 需要以特定服务账号（而非 SYSTEM）运行并纳入服务权限审计。

**换法二选一**：
(a) 写一个 C++ 真服务壳（`StartServiceCtrlDispatcher` + `CreateProcess` 拉起
`ems_supervisor.py`），壳本身零依赖，符合约定 §1 —— **推荐**；
(b) 用一个已实现 SCM 握手的第三方包装器（破坏零依赖，**不推荐**）。
两条路的参数与前置条件都写在 `install_service.bat` 的注释与 `--recipe` 输出里。

### 2.2 为什么守护器要自己写，而不是靠外面的"失败重启"

因为 `11/` §2.1 有三条**部署顺序约束**，任何"外面包一层"的方案都绕不过去：

1. **初始化器必须最先跑，且必须常驻。** RT_DB 共享内存段是 Windows 页面文件映射对象，
   **最后一个句柄关闭即销毁**。初始化器跑完就退 = 段没了、数据也没了。
   （它的 `reset=true` 还会 `memset` 整段，所以它必须在任何进程之前。）
2. **设备侧要在段建立之后再连**，否则连到一个不存在的段。
3. **EMS 侧要在设备侧发布 CFG 之后才连**，否则读不到配置（现场报
   `device side not publishing`）。

于是守护器的设计落成四件事：

- **依赖 DAG + 就绪探测**：每个进程配 `depends_on` 与 `ready_pattern`，
  上游**日志里真的出现**就绪行才起下游 —— **不是** `sleep N` 猜时间。
  超时（`ready_timeout`）视为**启动失败**，直接 kill 重启，而不是继续等。
- **退避**：`min(cap, base × factor^(连败-1))`，稳定运行 `stable_s` 后退避计数清零。
  防"起来就崩 → 立刻重启 → 再崩"打满 CPU。
- **级联作废（传递闭包）**：上游死了/超时了，下游一律 kill 并作废，
  等上游重新就绪后再按序拉起。避免出现"EMS 还连着已经不算数的旧状态"。
- **preflight**：`check` 子命令先验"可执行文件齐不齐、依赖成不成环"，
  现场装之前就能发现问题，不必等到开机后看日志。

### 2.3 配置版本：目录即真相源，`index.json` 只当备注

`<安装根>/conf/ems_config.json` 是生效值，`<安装根>/conf/history/` 是历史版本
（`vNNNN_YYYYMMDD-HHMMSS.json`）。四条判据对应的设计：

- **逐字段可比**：落盘一律 canonical JSON（`sort_keys=True, indent=2`，尾换行），
  所以任意两版做字符串比较即逐字段比较；比较前先 `flatten()` 成
  `safety.soc_min` 这样的路径，diff 能指出"**哪个字段**从 X 变成 Y"，
  而不是"整个 `safety` 节变了"。
- **空节算叶子**：`flatten` 把**空 dict/list 本身**也当叶子。否则"把
  `strategies.S04_DEMAND_MGMT.params` 整节删掉"会被报成 0 处差异 —— 这是漏报。
- **原子写**：`tmp + os.replace`，断电/被杀不会留下半个 JSON 当生效值。
- **回滚前先自动备份**：`rollback(v)` 的第一步是 `snapshot(source=当前生效值)`，
  第二步才写。所以回滚**永远让版本数 +1**，不是 -1；"回滚错了"本身可再回滚。
- **`index.json` 是 advisory**：它只存备注（`note`）。`list` / `diff` / `rollback`
  的真相源是**目录扫描**，`index.json` 坏掉或被盗删不影响这三条。
- **`resolve()` 接受多种写法**：`latest` / `active` / `v0007` / `7` /
  `list` 直接打印出来的全名 —— 方便现场复制粘贴。

### 2.4 日志轮转：先回答"会不会丢"，再谈轮转

任务书点名的那个问题：**P2 的 `SoeLog` 默认有界 4096 条，长跑时会丢最旧的。**
`log_rotator.py` 不能只做"按大小切文件"，必须先给出**不丢事件的算式**：

```
导出周期 T（秒） × 最大事件生成速率 r（条/秒） < 4096
```

即 `T < 4096 / r`。`advise(r, T)` 就是这条算式的实现：它算出
`max_interval = capacity / r`，建议值取**一半**（留一倍余量），
并判定当前 `T` 是否**严格小于**边界（`used < capacity`，不是 `<=`）。
`ingest` 在发现 `dropped() > 0` 时打印 `[LOST]` 并以**退出码 2** 收尾 ——
丢事件是硬失败信号，不能被"导出成功"掩盖。

轮转本身按**大小 + 条数双阈值**：任一超限即封存当前段。
双阈值缺一不可 —— 只按大小，遇到"条数多但每条极短"会把内存吃光；
只按条数，遇到"单条 message 极长"会写出巨大文件。
保留 `keep` 份、超出删最旧；封存段名带序号，`query(t0,t1,level,code,source)`
按时间范围跨段检索。

---

## 3. 踩坑记录

### 坑 1 `.bat` 里中文紧邻 `!VAR!` 会**静默吃掉变量**

- **现象**：`echo 安装根 !ROOT! 就绪` 打印出来是"安装根  就绪"，
  `!ROOT!` 整段消失；**退出码仍是 0**，什么都不报。
- **根因**：cmd.exe 在 CP936 下扫描延迟展开时，中文双字节的前导字节会与相邻
  字节配对，`!` 的边界判定出错。
- **修法**：**含 `!VAR!` 的行保持纯 ASCII**；中文只出现在**不含变量**的
  `echo` / `REM` 行。测试 `test_scripts.py` [B] 段把这条变成硬断言。
- **判据有效性**：拿一行 `echo 安装根 !ROOT! 就位` 喂给检测逻辑，必须报 1 处；
  同一行去掉中文（`echo root !ROOT! ready`）必须报 0 处 —— 证明它不是全盘误报。

### 坑 2 `.bat` 的中文 `echo` 行后面的**数字也会被吞**

- **现象**：汇总行 `echo ... PASS=!TOTAL!` 前面带中文时，`PASS=` 后面的数字
  打印出来是空的，`findstr` 抓不到 → 全量汇总静默变成 0。
- **根因**：同坑 1 的字节配对问题的另一面。
- **修法**：**机器要读的数字必须单独占一行、且该行纯 ASCII**。
  `build_test.bat` 里汇总行写作
  `echo      assertions total: PASS=!TOTAL!  FAIL=0  SKIPPED=!TOTSKIP!`（纯 ASCII），
  中文说明行一律不含变量。测试 [F] 段断言该行全 ASCII 且 `PASS=` 后直接跟变量。

### 坑 3 从 Git Bash 直接跑 `.bat` 会被仓库路径里的括号坑到

- **现象**：从 Git Bash `./19/scripts/install_task.bat` 各种诡异失败。
- **根因**：仓库根是 `D:\wb(cn)\BMS`，`cmd.exe` 解析块结构时把值里的 `(cn)` 当
  子表达式括号，整个脚本在**执行任何一行之前**就报
  「此时不应有 \BMS\...」并以 255 退出（这也是 `scripts/fix_bat_encoding.py`
  会检查"块内路径变量 `%CD%` 展开"的原因）。
- **修法**：一律用 **Python 起子进程** `subprocess.run(["cmd","/c", relpath], cwd=REPO)`；
  `.bat` 内部刻意只用**裸文件名 + 相对路径**做参数，并把所有读路径的分支改成
  `goto` + 标签（不用 `if (...)` 块）。
- **判据有效性**：`fix_bat_encoding.py --check` 对 6 个脚本报"全部合规"；
  测试 [A] 段还反向构造一个纯 LF 的 `.bat`，要求检查器必须判红（rc=1）。

### 坑 4 `install_task.bat --dry-run` 走成了**真安装**

- **现象**：本想演练，结果 rc=1 且输出 `need_admin`。
- **根因**：分支判据写成了 `if "!DRY%"=="1"` —— `!DRY%` 是笔误，`!` 与 `%` 不配对，
  比较**恒不相等**，于是 `--dry-run` 被无视，直接往下走到安装段。
- **修法**：字节级替换 `!DRY%` → `!DRY!`。
- **判据有效性**：测试 [C] 段真跑 `--dry-run`，断言 rc=0、输出含 `[DRY-RUN]` 与
  `nothing was changed`，且**不含** `scheduled task registered`。

### 坑 5 `--dry-run` / `--check` 有**副作用**（会写盘）

- **现象**：只想看一下，跑完安装根下多出了 `conf/`、`log/` 与
  `conf/supervisor.json`。
- **根因**：脚本的写盘动作排在"判断是否 dry-run"**之前**。
- **修法**：重排 —— `check` 提到写盘之前（清单不存在时守护器用内置默认，
  所以 `check` 不需要先写文件）；`--check` / `--dry-run` 分支在**任何 `mkdir` 之前**
  `goto` 返回。`run_supervisor.bat` 也做了同样改造（删除 `mkdir` 与 `--write-config`，
  变成纯 `plan`/`check`）。
- **判据有效性**：测试 [C] 段在跑整段前后各取一次安装根顶层条目快照，要求
  **一字不差**；并单独断言 `conf/supervisor.json`、`log/supervisor.log`
  没有被创建；另加两条**静态**不变量断言：`goto check_only` / `goto dry_run_task`
  的**行号必须小于**第一个 `mkdir` 的行号。

### 坑 6 卸载 `--dry-run` 撞上环境黑名单

- **现象**：`uninstall_task.bat --dry-run` 在不允许管理计划任务的机器上失败 ——
  即使只想演练。
- **根因**：演练分支读完后仍执行了一条 `schtasks /Query`（"查一下在不在"），
  而该环境下 `schtasks.exe` 被策略拦截。
- **修法**：把 `--dry-run` 分支移到**任何 `schtasks` 调用之前**返回；
  并加 `where schtasks` 前置检查（缺则走 `:no_schtasks` 明确报错），
  顺带改善现场可用性。
- **判据有效性**：测试 [C] 段用 `first_exec_line`（要求行**以** `schtasks` 开头，
  排除 `set`/`echo` 里的字符串）定位真实调用行，断言 dry-run 的 `goto` 行号在它之前。

### 坑 7 stdin 重定向到 NUL 会让守护器**秒退**

- **现象**：`ems_supervisor.py run > nul < nul` 打印完 banner 就退出。
- **根因**：Windows 上 `isatty(NUL)` 返回**真**，而 stdin 指向 NUL 时读到的是
  立即 EOF。看门狗误判"控制台没了"。
- **修法**：**不看 `isatty`**。看门狗**默认关闭**，只有显式设
  `EMS_SUPERVISOR_STDIN_WATCHDOG ∈ {1,yes,true,on,stdin}` 才启用。
  `run_supervisor.bat` 也**不做任何 stdin 重定向**并写了大字注释。
- **判据有效性**：测试 [F] 段跑两个对照，默认（stdin=NUL）必须活满全程，
  显式开启（stdin=NUL）必须秒退 —— 两条都要成立，才说明"默认关"是真的。

### 坑 8 浮点边界把一条事件漏掉了

- **现象**：查 `[0.0, 1.9]` 只命中 19 条，事件流明明是 20 条（`t = k × 0.1`）。
- **根因**：`19 × 0.1 == 1.9000000000000001 > 1.9`，严格 `<=` 比较把它排除。
- **修法**：`log_rotator.py` 引入 `T_EPS_S = 1e-6`，`query()` 用
  `lo = t0 - eps` / `hi = t1 + eps`。
- **判据有效性**：测试里配一条**反向守卫** —— 无容差的实现只命中 19 条，
  证明这条容差不是"随便放宽"。

### 坑 9 `sort_keys` 会改变"第一个键"

- **现象**：断言"canonical JSON 首键是 `description`"失败，实际是 `coordinator`。
- **根因**：`canonical()` 用 `sort_keys=True`，`c` 排在 `d` 前。
- **修法**：断言改成"顶层键为字典序、且 `keys[0] == "coordinator"`"。
- **判据有效性**：这类"按实现细节写死期望"的断言是**假红源**，改正后它测的是
  "键序确定"这个**真契约**。

### 坑 10 测试夹具里 `tmp ≠ root`

- **现象**：守护器测试 [D] 段全红 + `IndexError`：假子进程找不到。
- **根因**：`write_root()` 把假脚本写在 `root` 里，调用方却按 `tmp` 拼路径传参。
- **修法**：`write_root()` 支持 `@FAKE@` 占位符（替换成真实路径），
  调用方一律写 `"@FAKE@"`。
- **判据有效性**：这是**测试自身的 bug**，不是被测代码的。修它是为了让
  "全红"不被误读成"实现坏了"。

### 坑 11 Python 子进程冷启动约 0.8 s，顺序用例会偶发

- **现象**：顺序类用例偶发"上游已退出、下游还没起来"。
- **根因**：假子进程只活 1.0 s，而 Python 子进程在本机冷启动约 0.8 s。
- **修法**：`write_root()` 默认 `alive=30`（**长命**）；需要短命的用例（退避/级联）
  显式给存活时间，并把观察窗口放宽到 8 s。
- **判据有效性**：把"默认长命"写进夹具，是为了让"顺序"类断言只测顺序，
  不测启动速度。

### 坑 11b 退出码用例曾用 1.6 s 窗口 —— 满载时冷启动吃穿窗口，断言偶发假红

- **现象**：`[C] 从未就绪的进程反复退出 → 运行期失败（退出码 2）` 在**全量构建**
  （机器满载）下偶发 `得到 0，期望 2`；单跑常常全绿。
- **根因**：该用例的短命子进程 `alive=0.2 s`，但**真 Python 冷启动约 0.73 s**
  （实测 10 次：0.696~0.771 s，均值 0.73 s），满载时更慢。于是首拍退出 ≈0.9 s、
  退避 0.3 s 后第二拍启动 ≈1.2 s；而观察窗口只有 **1.6 s**。满载下首拍还没退出
  窗口就关了 → `starts=1` → `restarts=max(0,1-1)=0` → `bad` 为空 → **rc=0**。
  守护器逻辑没错（它一旦观测到 2 次启动且从未 ready 就正确返回 2），
  是**测试窗口比冷启动还紧**。
- **修法**：窗口 1.6 s → **4.0 s**，保证至少 2 个完整 start-exit 周期
  （最坏冷启动 1.5 s 时：首拍退出 1.7 s、第二拍启动 2.0 s，仍在窗口内）。
- **判据有效性**：修后单跑连续 `PASS=73 FAIL=0`。**注意**：这类"短命 + 短窗"的
  组合是本模块最容易漏的时序陷阱 —— 凡是用 `alive` 控制短命的用例，窗口都要按
  `冷启动(≤2.0 s) + alive + 退避 × 跳数` 再留余量，不能只按 `alive × N` 拍脑袋。

### 坑 11c 本机冷启动已涨到 1.4~1.9 s —— [D] 三跳链窗口再次吃紧（2026-10-07）

- **现象**：全量构建 `19/` 从 **424 → 423**，唯一红项
  `[D] 反向守卫：ems 先成功起来过`；单跑 3 次仍 100% 复现（不是偶发）。
- **根因**：`[D]` 是**三跳链** `init → dev → ems`，每跳都要付一次解释器冷启动。
  2026-10-07 实测本机冷启动 **1.42 ~ 1.85 s**（10 次采样），早已超过本文件早先
  记的 0.8 s、以及坑 11b 里按的"上限 1.5 s"。实测排程：
  `init` ready t=1.30 → `dev` start 1.58 / ready 3.10 → `ems` start 3.39，
  而 `init`（当时 `alive=2.5 s`）**t=3.89 就退出** → `ems` 还没 ready 就被
  `cascade_dirty` + `kill` 掉。于是守卫「ems 先成功起来过」必红。
  守护器本身的级联语义是**对的**（日志里 `cascade_dirty name=dev`、
  传递闭包到 `ems`、随后 `start name=init attempt=2` 重启链完整）。
- **修法**：按 冷启动上限 2.0 s × 3 跳 重标定 —— `init` 存活 **2.5 → 8.0 s**、
  观察窗口 **8.0 → 20.0 s**。最坏情况 ems 就绪 ≈ 6.7 s、init 退出 ≈ 9.9 s，
  余量 3.2 s；退出后的重启链（再 3 跳）也仍在 20 s 内。
- **判据有效性**：修后单跑连续 **`PASS=73 FAIL=0`**（3/3），`19/` 合计回到 **424**。
- **教训**（比坑 11b 更狠的一条）：**链上每多一跳，就等于多乘一次冷启动**。
  所以窗口不能按"一个冷启动"估，要按 **跳数 × 冷启动上限** 估；而"冷启动"
  这个常数本身会随机器负载 / 杀软 / 解释器数量漂移，**隔一段时间必须重测**，
  不能把某次实测值当成永久常量写死在注释里。

---

## 4. 测试清单

| 套件 | 覆盖 | 关键断言（含反向守卫） |
| --- | --- | --- |
| `test_config_store.py`（86） | snapshot / list / diff / rollback / verify / resolve；canonical 往返；空节算叶子 | ① 快照往返**逐字段一致** ② diff 指出**哪个字段**从 X 变成 Y（10 条差异逐条写死）③ 回滚后生效值 == 目标版本 ④ **回滚前自动备份**（版本数 **+1** 而非 -1）· 反向守卫：`naive_toplevel_diff()` 这个"只比顶层键"的错误实现必须报不出点号路径 · [G] 真调 `P1/build/ems_config.exe --capture`（缺 exe → 显式 SKIP） |
| `test_log_rotator.py`（84） | 轮转阈值 / 保留份数 / 不丢事件 / 时间范围检索 / `dropped` 接漏 / metrics / 算式边界 / CLI | ① 超**大小**阈值触发轮转 ② 保留 `keep` 份、多余的删最旧（`soe_0004/0005`）③ **不丢事件**：N=200 确定性事件流导出后逐条可比 ④ 轮转后仍能按时间范围查到旧事件 · 反向守卫：无浮点容差的实现只命中 19/20 条 · 列名与 P2 `soe.h` 的 `csv_header()` 交叉比对 · 算式 `T×r<4096` **边界两侧**各测一次 · CLI 子进程端到端（`dropped>0` → rc=2） |
| `test_supervisor.py`（73） | preflight / 启动顺序 / 退避 / 级联 / 就绪超时 / stdin 看门狗 / CLI / 真产物端到端 | 依赖永不就绪时**下游一个都不许起** · 退避间隔严格递增 · 级联作废（传递闭包）· 就绪超时 = 启动失败（**不是** sleep）· stdin 看门狗**默认关闭**（默认+NUL 活满全程 ↔ 显式开启秒退）· [H] 段用 `11/` 真三进程端到端：`rtdb_initializer → device_side → ems_side` 依次就绪、0 restart、0 cascade、EMS 日志里没有 `device side not publishing` |
| `test_scripts.py`（181） | 6 个 `.bat` 的齐套 / 行尾 / 编码；dry-run 真跑；指导书齐套；点表逐点对账；汇总契约 | 全部 `.bat` 过 `fix_bat_encoding.py --check`（含**反向守卫**：LF 文件必须判红）· 含 `!VAR!` 的行全 ASCII（含反向守卫）· `install_task --dry-run/--check`、`uninstall_task --dry-run`、`install_service --recipe`（rc=2）、`run_supervisor plan/check` **真跑**且**零副作用** · **点表 32 点与 `13/ --dump-tsv` 逐点对账**（含反向守卫：改一个点名恰好报 1 处）· `call :run` 集合 == `tests/test_*.py` 集合（不重不漏）· 汇总行纯 ASCII |

测试风格说明：不用 `pytest` / `unittest`。统一用 `19/tests/harness.py` 的
`T` 收集器，收尾行固定为

```
PASS=n FAIL=m SKIPPED=k
```

格式是**契约**（行首 `PASS=`、单个空格分隔）——`build_test.bat` 用
`findstr /b "PASS="` 抓这一行做累加，多一个前导空格就抓不到，全量汇总会静默变成 0。

---

## 5. 已知边界

**诚实优先 —— 下面是"没有做"，不是"已完成"。**

1. **没有真·Windows 服务。** `install_service.bat` 只是**配方**，退出码 2 是
   **故意**的（见 2.1）。C++ 服务壳没有实现。
2. **计划任务的实际"开机自启"没有在真机上验证过。** 本施工环境没有管理员权限，
   且 `schtasks.exe` 在策略黑名单里（无法执行、也不可绕过）。已验的是：
   `--dry-run` / `--check` 真跑、命令行拼装正确、前置检查通过、零副作用。
   **未验**：`/SC ONSTART /RU SYSTEM` 在真机重启后确实拉起守护器。
3. **守护器不做资源监控与告警外发。** 没有 CPU/内存/磁盘门限，
   没有邮件/短信/Webhook 通知 —— 现场靠人看日志与 `11/` 自身的告警。
4. **`log_rotator` 的 metrics 轮转按 Prometheus 文本的"行数"计条数**，
   不做指标聚合、不做降采样。它保的是"原文不丢"，不是"曲线好看"。
5. **配置的字段语义校验依赖 `P1/`。** 19/ 不重复实现 89 字段的量程/类型/依赖校验；
   `config_store` 只保证"落盘、可版本化、可回滚"。P1 不在场时，
   配置的正确性没有兜底。
6. **没有挂进 `scripts/build_all.bat`。** 按约定 §6 这一步**由集成方统一执行**
   （避免并发改同一文件互相覆盖），且本次施工的硬性边界是
   **不改 `scripts/build_all.bat`**。因此 19/ 目前只在自身
   `19/scripts/build_test.bat` 下跑。
7. **点表核对只到"文档 ↔ 13/ 可执行点表"这一层。** 真机厂家点表没接 ——
   那需要设备手册与现场读数，不在本模块能力内（指导书 §4.1 给了核对方法与记录表）。
8. **回滚是本机的，不跨站。** `conf/history/` 是安装根下的目录，
   没有"从别的站拷一版配置过来"的机制，也没有配置的签名/防篡改。
9. **不做进程优先级 / CPU 亲和性 / 实时性设置。** 守护器只保"进程活着 + 顺序对"。
10. **19/ 不保证控制律正确。** 它保的是"三个进程按顺序在跑、日志能查、
    配置能回滚"。功率算得对不对是 `05`~`11` / `P*` 的责任。
11. **只有 Windows 路线。** 脚本是 `cmd` 语法；没有 Linux `systemd` 单元文件。
12. **`install_service.bat` / `uninstall_service.bat` 未做端到端验证**
    （缺 SCM 包装器，本环境也无法注册服务）。它们的状态是"配方 + 前置检查"，
    不是"可一条命令装好"。

---

## 6. 目录结构、依赖方向与编译参数

```
19/
├── src/
│   ├── ems_supervisor.py        进程守护器：DAG 顺序 + 就绪探测 + 退避 + 级联作废
│   │                            CLI: run --duration / check / plan
│   │                            --dump-config / --write-config
│   ├── config_store.py          配置落盘与版本：snapshot / list / show / diff /
│   │                            rollback / verify；canonical JSON + 原子写
│   └── log_rotator.py           SOE/metrics 落盘与轮转：大小+条数双阈值、
│                                keep 保留、时间范围检索、advise(T×r<4096)
├── tests/
│   ├── harness.py               最小断言收集器 T（收尾行格式即契约）
│   ├── test_config_store.py     86
│   ├── test_log_rotator.py      84
│   ├── test_supervisor.py       73
│   ├── test_scripts.py         181
│   └── fixtures/
│       ├── ems_config_site_v1.json   105 个叶子
│       └── ems_config_site_v2.json   与 v1 有 10 处差异（含整节删除+新增）
├── scripts/
│   ├── build_test.bat           跑 4 个套件并汇总（findstr /b "PASS="）
│   ├── install_task.bat         注册开机自启计划任务（--dry-run / --check）
│   ├── uninstall_task.bat       停止并删除计划任务（--dry-run）
│   ├── install_service.bat      Windows 服务路线（**只给配方**，rc=2）
│   ├── uninstall_service.bat    服务路线卸载（配套）
│   └── run_supervisor.bat       前台跑守护器（plan / check / run，零副作用）
├── docs/
│   ├── README.md               本文件
│   └── 现场作业指导书.md         P5 核心：从空服务器到接上真实位号的可复现步骤
└── build/                       测试产物与临时目录（不入库）
```

**依赖方向**：`19/` 不 include / import 任何兄弟模块的源码。
它对 `11/`、`P1/`、`P2/`、`13/` 的依赖全部是**进程级**或**文件级**的
（起进程、读日志行、调 exe、读/写 JSON 与 CSV），且一律**单向**：
`19/ → {11, P1, P2, 13}`，没有反向引用。

**编译参数**：本模块**没有 C++ 编译**，因此不涉及 `-std=c++17 -Wall -O2`。
它只要求：

- **Python 3**（本机 `3.13.14`），**仅标准库** —— 无 `requirements.txt`、
  无 `pip install`。`subprocess` / `json` / `os` / `re` / `argparse` 等。
- **`cmd.exe` + `schtasks.exe`**（系统自带）。
- 现场可选：`13/.venv`（若存在，脚本优先用它当解释器；不存在则退回 PATH 上的
  `python`，再不行可用环境变量 `EMS_PYTHON` 指定）。

跑法与汇总：

```bat
19\scripts\build_test.bat
```

末行为
`[OK] 19\ P5 site deployment: 4 test suite(s) all passed` 与
`assertions total: PASS=424  FAIL=0  SKIPPED=0`，并落一份**纯 ASCII** 的
`19/build/test_status.txt`（供上层脚本判定，不要靠 grep 中文输出）。
