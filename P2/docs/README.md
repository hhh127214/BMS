# P2 · 可观测性

> 产品化路线第 4 步（P0 架构分层 ✅ → P0.5 适配器可换性 ✅ → P1 配置化 ✅ → **P2 可观测性** → P3 通信 → RT_DB 接入）
>
> **目标：现场出问题时，能在 30 秒内回答"发生了什么、什么时候、多严重"，而不是去翻 8 万行时序 CSV。**

---

## 1. 为什么需要 P2

P1 解决了"不改源码就能适配现场"。但现场跑起来之后，下一个问题是**看不见**。
P2 之前，可观测性散在三个模块、三种表示，而且**在生产环境不可用**：

| 位置 | 表示 | 问题 |
|---|---|---|
| `06/state_machine.h` | `StateEvent{ts, from, to, reason}` | 只有状态迁移，没有等级 / 事件码 |
| `10/src/sim_24h.h` | `AlarmEntry{t, level, source, message}` | **告警组装寄生在仿真装配层** —— `collect_alarms()` 的入参是 `Sim24hConfig`（仿真专属结构体），现场没有它。**也就是说现场部署后系统没有告警能力。** |
| `07/realtime_loop.h` | `StepRecord` 向量 | 是时序，不是事件；而且 `LoopMetrics::compute(log_, …)` 是 **O(N) 全量重算** |
| `07/realtime_loop.h` | `cycle_us_sum_ / max_` | 只有均值与最大值，**没有直方图 / 分位数** |

三个具体缺陷，都是生产环境会出事的：

### ① 事件风暴没有框架级抑制

周期 10 实测：收紧配置下 `safety_clip` 连续 **838 拍**。逐拍记事件就是 838 条。
`10/` 里手工实现了"记首次 + 恢复"，但**每个指标都要手写一遍**，漏一个就退化回风暴：

```cpp
bool in_reverse = false, in_tr_over = false, in_soc_hi = false, in_soc_lo = false;
bool in_clip = false;
// … 下面每个 if 都要自己维护对应的 in_xxx 标志，重复 5 遍
```

### ② 指标必须保留全部 StepRecord，内存只增不减

```cpp
LoopMetrics metrics() const {
    const int stride = cfg_.log_every < 1 ? 1 : cfg_.log_every;
    LoopMetrics m = LoopMetrics::compute(log_, cfg_.dt_s * stride);   // O(N)！
}
```

24 h @ dt=1 s = 86400 条 × 约 100 B ≈ 8.6 MB，**只增不减**。现场连续跑一年就是 3 GB。
而且 `metrics()` 每次 O(N) 重算 —— 不可能每秒调一次给监控系统。

### ③ `log_every` 一个旋钮承担两个冲突的职责

| 职责 | 想要的方向 |
|---|---|
| **保真**：现场要能回放故障 | `log_every = 1` |
| **省资源**：86400 拍全记就是 8.6 MB/天 | `log_every = 100` |

周期 10 已经吃过一次亏：**倒送缺陷在 `log_every=10` 下显示 0 违规，`log_every=1` 才暴露。**
**为了省资源而调大 `log_every`，等于同时关掉了故障可见性。**

---

## 2. 模块结构

```
P2/
├── src/
│   ├── soe.h        统一事件记录（SOE: Sequence of Events）+ 时间窗抑制
│   ├── metrics.h    增量指标注册表（counter / gauge / histogram）
│   ├── trace.h      跟踪等级 + 采样间隔（按子系统独立）
│   ├── observe.h    RuntimeObserver —— 接入 EmsRuntime 的唯一入口  ← 核心
│   └── main.cpp     演示程序（ems_observe.exe）
├── tests/
│   └── test_observe.cpp   T301 ~ T315（434 条断言）
├── docs/
│   └── README.md    本文件
└── scripts/
    ├── build.bat / build_test.bat / run_demo.bat
```

**关键约束：不修改 `07/`。** 观察者从外部喂 `StepRecord`：

```cpp
EmsRuntime rt;  rt.init();
RuntimeObserver obs;
obs.start(0.0);
for (...) {
    rt.set_environment(load, pv);
    StepRecord rec = rt.step(dt);
    obs.on_step(rt, rec);          // ← 唯一的接入点
}
obs.stop(t);
```

理由：可观测性不该侵入被测对象。`07/` 的 `step()` 已经是"一次闭环"的完整语义，
观察者只读它的输出 + 只读访问 `rt` 的公开状态。

---

## 3. 两个核心机制

### ① 边沿检测 → SOE：事件是"状态的变化"，不是"每拍的值"

一次持续 838 拍的安全限幅，物理上是**一件事**，应该产生 **2 条**事件（Start / End），
不是 838 条。所以观察者保存上一拍的布尔量，只在跳变时记事件 —— 收敛成一张 `edge_state_` 表，
替代了 `10/` 里手工维护的 5 个 `in_xxx` 标志。

**例外：状态迁移与硬不变量违例不参与抑制（`suppressible = false`）。**
合并它们会丢掉迁移链 —— `A→B→C` 会变成 `A` 重复 2 次，而迁移链正是要留的：

```cpp
// soe.h
bool suppressible = true;   // false 用于"每次都独立成条"的事件
```

状态迁移另外还有一个考虑：**不自己推导，直接读状态机的权威记录 `rt.fsm().history()`**。
理由是迁移自带 `reason`（现场要的是"为什么迁"，不是"迁到哪"），而且自己用 `prev_state_`
做边沿检测会**漏掉第一拍的 `INIT → SELF_CHECK`**（被当成"初始状态"吃掉）。

### ② 时间窗抑制：同 `(source, code)` 在窗口内合并

```
一次 838 拍的限幅  →  1 条 SAFETY_CLIP_START(repeat_count=838, duration_s=837)
                  +  1 条 SAFETY_CLIP_END
```

窗口从**上一次发生时刻**起算（滑动窗口），所以持续数小时的风暴会一直合并成一条，
`duration_s()` 就是风暴时长。合并时 `merge_data()` 保留**绝对值更大**的字段值，
保证抑制后不丢峰值信息（例如最大越限幅度）。

### ③ 逐拍累积 → 指标：O(1) 更新，与 `log_every` 无关

指标走自己的累积路径，**不经过日志降采样**。这一点对下面两个量是决定性的：

- `cmd_travel_kw = Σ|Δcmd|`（指令总行程）
- `cmd_reversals`（方向反转次数）

降采样会**漏掉中间的抖动** —— 而"抖动"恰恰是它们要度量的东西。
`--decouple` 演示给出了实测证据：

```
场景：3600 拍，负荷在 300/50 kW 之间每 5 拍切换（持续充放换向）

  log_every   观察者行程 观察者换向 日志行数 07/ 日志行程
  ----------------------------------------------------------
  1              88477.8        719       3600      88477.8
  10             88477.8        719        360       0.0

① 观察者：log_every 1 → 10，行程 88477.8 → 88477.8 kW  （完全一致 ✓）
② 07/ 日志口径：行程 88477.8 → 0.0 kW  （降采样后严重失真 ✗）
③ 日志行数 3600 → 360（省了 10 倍存储，代价是 07/ 的指标失明）
```

`log_every=10` 时 `07/` 报的行程是 **0**，真实值是 **88477.8 kW** —— 完全失明。
观察者不受影响。

---

## 4. 三个正交的旋钮（替代单一 `log_every`）

| 维度 | 载体 | 现场用法 |
|---|---|---|
| ① **关键事件** | `SoeLog` | 永远记，不受任何降采样影响。频率天然很低（838 拍限幅 → 2 条） |
| ② **跟踪等级** | `TraceControl::set_level()` | 按子系统决定"要不要输出细节"。排查某子系统时把它调到 `kDebug`，其他不受影响。运行期热更新，不用重启 |
| ③ **采样间隔** | `TraceControl::set_sample_every()` | 高频跟踪按子系统采样。与 `log_every` 的区别是**按子系统独立**，且**不影响 SOE** |

**故障级（`>= kError`）永不受等级限制**：

```cpp
bool enabled(SoeSource s, SoeLevel lv) const {
    if (lv >= SoeLevel::kError) return true;   // 故障级别**永不受等级限制**
    return lv >= level(s);
}
```

否则会出现"现场把日志等级调高省资源，结果把故障也调没了"这种事故。

---

## 5. 事件分级

### 等级（可比较、可统计）

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `kDebug` | 调试细节（默认关） |
| 1 | `kInfo` | 正常事件（状态迁移 / 恢复） |
| 2 | `kWarn` | 预警（降功率 / 接近限值） |
| 3 | `kError` | 故障（门控 / 通信中断） |
| 4 | `kFatal` | 致命（不可继续运行） |

### 来源（9 个子系统）

`SYSTEM` / `FSM` / `SAFETY` / `COORD` / `STRATEGY` / `DEVICE` / `COMM` / `CONFIG` / `ECON`

### 事件码（成对的 Start / End）

命名约定 `Start` / `End` 成对出现 —— 这是"边沿检测 + 抑制"的直接产物。

| 区段 | 示例 |
|---|---|
| 100–102 | `FSM_TRANSITION` / `FSM_EMERGENCY_STOP` / `FSM_RUN_PERMIT` |
| 200–215 | `SAFETY_CLIP_START/END` / `GRID_REVERSE_START/END` / `TR_OVERLOAD_START/END` / `SOC_LOW_START/END` / `SOC_HIGH_START/END` / `TEMP_HIGH_START/END` / `RAMP_LIMITED_START/END` |
| 300–311 | `COMM_LOST_BMS/METER/PCS` / `COMM_RESTORED` / `DATA_STALE_START/END` / `PCS_FAULT_SET/CLEAR` / `DEVICE_OFFLINE/ONLINE` / `HOLD_LAST_START/END` |
| 400–402 | `REOPT_FIRED` / `PLAN_INVALID_START` / `PLAN_VALID_END` |
| 500–501 | `DEMAND_BREACH_START/END` |
| 900–903 | `OBSERVER_START/STOP` / `BUFFER_OVERFLOW` / `INVARIANT_BROKEN` |

**清除事件码必须与置位事件码配对**，否则现场看到"PCS 故障置位"却收到"通信恢复"会直接误判根因：

| 故障位（`FaultSet::bits()`） | 置位事件 | 清除事件 |
|---|---|---|
| 0 `bms_comm_lost` | `COMM_LOST_BMS` | `COMM_RESTORED` |
| 1 `pcs_comm_lost` | `COMM_LOST_PCS` | `COMM_RESTORED` |
| 2 `meter_comm_lost` | `COMM_LOST_METER` | `COMM_RESTORED` |
| 3 `pcs_fault` | `PCS_FAULT_SET` | **`PCS_FAULT_CLEAR`** |
| 4 `data_invalid` | `DATA_STALE_START` | **`DATA_STALE_END`** |
| 5 `device_offline` | `DEVICE_OFFLINE` | **`DEVICE_ONLINE`** |
| 6 `temp_fault` | `TEMP_HIGH_START` | **`TEMP_HIGH_END`** |
| 7 `emergency_stop` | `FSM_EMERGENCY_STOP` | `COMM_RESTORED` |

---

## 6. 有界内存与自省

可观测性组件**必须能报告自己的数据丢失**，否则比没有更危险 ——
"什么都没输出"到底是"真的没事"还是"缓冲区爆了"？

| 指标 | 含义 |
|---|---|
| `SoeLog::size()` | 当前存储条数（≤ `capacity`，默认 4096 条 ≈ 600 KB） |
| `SoeLog::dropped()` | 被容量**淘汰**的条数（超出上限即淘汰最旧） |
| `SoeLog::total()` | 累计 `push` 次数（含被抑制的） |
| `SoeLog::suppressed()` | 被时间窗**合并**的次数 |
| `TraceControl::filtered_total()` | 被等级/采样过滤掉的条数 |
| `TraceControl::passed_total()` | 实际输出的条数 |

`total() - size() - dropped()` 就是"被抑制合并"的量。三个数一起看才不会误判。

条目用 `std::list` 存储 —— 需要**稳定迭代器**（抑制时原地更新、淘汰时从头部删除）。
SOE 是事件级频率（每秒几条），不是拍级，所以链表较差的局部性在这里不是问题。

---

## 7. 导出

| 格式 | 用途 |
|---|---|
| `soe.csv` | 运维用 `grep` / `wc -l` 直接看。**一事件一行**（消息里的换行转义成字面量 `\n`，否则行数统计全线错位） |
| `soe.json` | 带 `summary` 统计，给上层平台解析 |
| `metrics.prom` | Prometheus 文本格式，直接被监控系统 scrape |
| `metrics.json` | 含直方图 `p50/p95/p99`，给人看 |
| `trace.json` | 当前跟踪配置与过滤统计 |

Prometheus 导出示例：

```
# HELP ems_steps_total 闭环步数
# TYPE ems_steps_total counter
ems_steps_total 21600
# TYPE ems_track_error_kw histogram
ems_track_error_kw_bucket{le="1"} 8123
ems_track_error_kw_bucket{le="10"} 21004
ems_track_error_kw_bucket{le="+Inf"} 21600
ems_track_error_kw_sum 24183.7
ems_track_error_kw_count 21600
```

---

## 8. 命令行

```bash
ems-observe --demo      [hours] [dt]   # 24h 闭环 + 观察者汇总（默认）
ems-observe --decouple                 # 指标与 log_every 无关（核心价值验证）
ems-observe --fault     [hours]        # 故障场景 SOE
ems-observe --export    <dir> [hours]  # 导出 soe.csv / soe.json / metrics.*
ems-observe --quiet                    # 只打印汇总
```

退出码：`0` 正常 / `1` 参数错误 / `2` 运行期失败（含硬不变量被破坏）。

`--fault` 实测输出（12 h，3 个故障时间窗，**18 条事件 / 43200 拍**）：

```
t_first    level source   code                   repeat  message
------------------------------------------------------------------------------
0.0        INFO  SYSTEM   OBSERVER_START              1  观察者启动
1.0        INFO  FSM      FSM_TRANSITION              1  状态迁移 INIT → SELF_CHECK (init_done)
6.0        INFO  FSM      FSM_TRANSITION              1  状态迁移 SELF_CHECK → READY (selfcheck_pass)
7.0        INFO  FSM      FSM_TRANSITION              1  状态迁移 READY → NORMAL (start_command)
8971.0     WARN  FSM      FSM_TRANSITION              1  状态迁移 NORMAL → DERATED (derate_enter:derated)
17281.0    ERROR FSM      FSM_TRANSITION              1  状态迁移 DERATED → FAULT (fault_in_derated:PCS_FAULT)
17281.0    ERROR DEVICE   PCS_FAULT_SET               1  故障位置位: PCS 故障
17881.0    INFO  DEVICE   PCS_FAULT_CLEAR             1  故障位清除: PCS 故障
17900.0    INFO  FSM      FSM_TRANSITION              1  状态迁移 FAULT → READY (fault_recovered)
19081.0    ERROR COMM     COMM_LOST_METER             1  故障位置位: 关口电表通信丢失
19086.0    ERROR COMM     HOLD_LAST_START             1  采集超时 → 保持上一拍指令
19381.0    INFO  COMM     HOLD_LAST_END               1  采集恢复
19381.0    INFO  COMM     COMM_RESTORED               1  故障位清除: 关口电表通信丢失
20881.0    ERROR FSM      FSM_TRANSITION              1  状态迁移 READY → FAULT (fault_in_ready:PCS_COMM_LOST)
20881.0    ERROR COMM     COMM_LOST_PCS               1  故障位置位: PCS 通信丢失
21181.0    INFO  COMM     COMM_RESTORED               1  故障位清除: PCS 通信丢失
21200.0    INFO  FSM      FSM_TRANSITION              1  状态迁移 FAULT → READY (fault_recovered)
43200.0    INFO  SYSTEM   OBSERVER_STOP               1  观察者停止
```

43200 拍 → 18 条事件，**2400:1 压缩**。每条故障都成对（置位 / 清除），
每条迁移都带 `reason`。

---

## 9. 测试清单

| 编号 | 内容 |
|---|---|
| T301 | 枚举：等级 / 来源 / 事件码 / 名称互转（含"每个码都必须有名字"遍历） |
| T302 | 时间窗抑制：838 拍风暴 → 1 条 Start + 1 条 End；窗口边界；关闭抑制 |
| T303 | 抑制例外：状态迁移链不被合并（`suppressible=false`） |
| T304 | 有界内存：容量淘汰 + `dropped` 如实上报 + 保留的是最新而非最旧 |
| T305 | 导出：CSV 引号/换行转义（一事件一行）、JSON 结构、过滤/查询 |
| T306 | 指标增量：counter / gauge（记极值）/ histogram（O(1)） |
| **T307** | **指标与 `log_every` 无关**（核心：行程 / 符号翻转 / 方向反转逐拍精确） |
| T308 | 直方图分位（含 `+Inf` 桶回落、空直方图、桶边界 `le` 语义） |
| T309 | Prometheus 文本格式 + 指标名净化 |
| T310 | 跟踪等级过滤：**故障级永不受限** |
| T311 | 采样间隔：按子系统独立；只有 `kDebug` 级走采样 |
| T312 | 观察者端到端：24h 默认场景 + 硬不变量 + 事件级压缩比 |
| T313 | 与 `10/` 结果交叉校验（`log_every=1` 时逐拍口径一致） |
| T314 | 故障场景事件：PCS 故障 / 电表通信 / PCS 通信 → 置位/清除事件对 |
| T315 | 观察者自身可观测：`suppressed` / `dropped` / `filtered` / `passed` |

T307 的关键断言（观察者 vs `07/` 日志口径）：

```cpp
// log_every=1：两者口径一致（验证观察者正确）
EXPECT_NEAR(o1.totals().cmd_travel_kw, n1.cmd_travel_kw, 1e-6);
EXPECT(o1.totals().cmd_reversals == n1.cmd_reversals);

// log_every=10：07/ 失明，观察者不受影响（验证观察者价值）
EXPECT(n10.cmd_travel_kw < 1e-9);
EXPECT(o10.totals().cmd_travel_kw > 1000.0);
```

T313 交叉校验：同一份 `Sim24hConfig` 分别跑 `run_sim_24h()`（10/ 装配层）与
观察者装配，断言关口极值 / 三项硬不变量 / 能量 / 状态迁移次数**完全一致**。

---

## 10. 现场怎么用

```bash
# 1. 上电前：跑一遍离线场景，确认硬不变量全通过
ems-observe --demo 24

# 2. 验证"指标不受降采样影响"（把结论写进验收报告）
ems-observe --decouple

# 3. 演练故障：确认置位/清除事件成对、门控正确
ems-observe --fault 24

# 4. 导出给监控系统 / 归档
ems-observe --export /var/log/ems 24
```

现场接入（生产代码）：

```cpp
RuntimeObserver obs;
obs.trace().set_global(SoeLevel::kInfo);          // 默认只看事件
obs.trace().set_level(SoeSource::kSafety, SoeLevel::kDebug);  // 排查安全层时临时放开
obs.start(rt.now());
while (running) {
    StepRecord rec = rt.step(dt);
    obs.on_step(rt, rec);
    // 每秒把指标推给监控系统 —— O(1) 取值，不是 O(N) 重算
    if (++tick % 1000 == 0) push_to_scada(obs.metrics().to_prometheus());
}
obs.stop(rt.now());
write_file("soe.csv", obs.soe().to_csv());        // 停机后归档事件
```

---

## 11. 与其它步骤的关系

```
P0  架构分层（IDeviceIO）      ← 让算法不知道底层是仿真还是现场
P0.5 点表适配器验证            ← 证明适配器真的可换
 ↓
P1  配置化                     ← 现场部署不改源码
 ↓
P2  可观测性                   ← 现场出问题能看见        ★ 本文件
 ↓
P3  通信（Modbus + IEC104）
 ↓
RT_DB 接入
```

| 依赖 | 说明 |
|---|---|
| 依赖 `07/realtime_loop.h` | 只读 `StepRecord` + `rt.fsm().history()` + `rt.safety_params()` / `rt.device_limits()` / `rt.config()` |
| 依赖 `06/state_machine.h` | 读 `EmsState` 与 `state_name()` |
| **不修改 `07/`** | 观察者是外挂的，`on_step()` 是唯一接入点 |
| 被 `10/` 复用（测试侧） | T313 用 `run_sim_24h()` 做交叉校验 |
| 被 P3 依赖 | 通信层的中断/恢复直接产出 `COMM_LOST_*` / `COMM_RESTORED` 事件 |

---

## 12. 编译

```bash
# 演示程序
g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src -I ../06/src \
    -I ../07/src -I ../08/src -I ../10/src src/main.cpp -o build/ems_observe.exe

# 单元测试
g++ -std=c++17 -Wall -O2 -I src -I ../04/src -I ../05/src -I ../06/src \
    -I ../07/src -I ../08/src -I ../10/src tests/test_observe.cpp -o build/test_observe.exe
```

或直接用脚本：`scripts\build.bat` / `scripts\build_test.bat` / `scripts\run_demo.bat`。
纯头文件实现（`soe.h` / `metrics.h` / `trace.h` / `observe.h` 全部 `inline`），无第三方依赖。
