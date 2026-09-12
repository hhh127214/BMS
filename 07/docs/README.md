# 07/ —— 周期 7：实时控制闭环

> 本模块是 [`工商业储能EMS调控策略设计方案.md`](../工商业储能EMS调控策略设计方案.md) §7 **周期 7** 的落地实现，
> 打通「电表实时数据 → EMS算法计算 → 策略仲裁 → 安全约束 → PCS执行 → 实际功率反馈 → EMS迭代修正」全链路。

| | |
| --- | --- |
| 对应周期 | 周期 7：实时控制闭环 |
| 周期目标 | 「实现毫秒级实时控制闭环，无延迟、无振荡。」 |
| 形态 | **纯头文件 C++17 库**（`src/plant_model.h` + `src/realtime_loop.h`）；`src/main.cpp` 为单场景演示 |
| 依赖 | `04/src`（策略层 + 仲裁器）、`05/src`（安全兜底）、`06/src`（状态门控）、`08/src`（协同层） |
| 单元测试 | `tests/test_realtime_loop.cpp`，**T11~T16 / 6050 断言，全过** |
| 演示 | `src/main.cpp` → 场景 C：阶跃跟随 + 抖动治理三档对照 + 变化率对照 |
| 关键常量 | 控制周期 `dt = 100 ms`；实时层纠偏上限 `l2_correction_max_kw = ±100 kW`；输出死区 `2 kW` / 滞环 `3` 拍 |

---

## 1. 快速开始

```bat
cd 07
scripts\build.bat          :: 编 test_realtime_loop.exe + loop_demo.exe
scripts\build_test.bat     :: 编 + 跑 T11~T16（应输出 PASS=6050 FAIL=0 / ALL TESTS PASSED）
scripts\run_demo.bat       :: 跑场景 C，输出 → build\demo_output.txt
```

> **编译依赖**：`src` + `../04/src` + `../05/src` + `../06/src` + `../08/src`。
> `EmsRuntime` 装配了 `08/` 的协同层，故需 `-I ../08/src`。这是**运行时调用关系**
> （闭环调用优化层），头文件层面**无环** —— `08/src/*.h` 不包含本模块任何头文件。

---

## 2. 文件导览

```
07/
├── src/plant_model.h              ← 被控对象：PCS 死区 + 一阶惯性 + 变化率 + 效率 + 温升 + 噪声/故障注入
├── src/realtime_loop.h            ← EmsRuntime 11 步闭环 + OutputShaper + LoopConfig/StepRecord/LoopMetrics
├── src/main.cpp                   ← 场景 C：3 个试验
├── tests/test_realtime_loop.cpp   ← T11~T16
├── docs/README.md                 ← 本文件
├── docs/design.md                 ← 设计说明（闭环时序 / 第⑨步顺序 / 整形语义 / 指标口径 / 抖动治理）
├── scripts/{build,build_test,run_demo}.bat
└── build/                         ← 产物（git ignore；`build\demo_output.txt` 为演示输出）
```

---

## 3. 闭环链路（`EmsRuntime::step()` 的 11 步）

```
电表实时数据 → EMS 算法计算 → 策略仲裁 → 安全约束 → PCS 执行 → 实际功率反馈 → EMS 迭代修正
```

| 步 | 动作 |
| --- | --- |
| ① | 采集：`plant_.sample(t_)` 产出**冻结**的 `RealtimeSnapshot`（接口规范 §5 强约束） |
| ② | 故障检测：`detect_faults()` |
| ③ | 安全评估：`safety_.evaluate()` → `SafetyVerdict` |
| ④ | 状态机：`fsm_.update()`（`06/`） |
| ⑤ | 协同层：`coord_.update()` → `plan_target` / `coord_target`（`08/`） |
| ⑥ | 策略层：`mgr_.tick()` + 安全结论并入（`04/`） |
| ⑦ | 仲裁层：`arbiter_.arbitrate()`（L0→L3 区间收敛 + L3 desired 加权） |
| ⑧ | 实时层纠偏叠加（L2 有界修正） |
| ⑨ | **输出整形 → 安全兜底 → 状态门控 → HOLD_LAST** |
| ⑩ | 执行层：`plant_.step()`（PCS 物理响应） |
| ⑪ | 反馈 + 记录：实测功率回灌下一拍快照，形成闭环 |

### 第 ⑨ 步的顺序（关键设计）

```
OutputShaper（死区/滞环去抖） → SafetyEngine::apply()（区间限幅 + 变化率限速）
  → 状态机门控（FAULT/EMERGENCY 强制 0，覆盖一切） → HOLD_LAST
```

- **整形放在最后一级**：`04/` 仲裁器的死区只作用于 L3 的 `desired`，而 L2 实时纠偏是在仲裁
  **之后**叠加的，抖动会绕过仲裁器死区。必须在链路末端再放一道整形。
- **安全层在整形之后**：安全兜底必须无条件生效，不能被整形"软化"。
- **状态机在安全层之后**：FAULT / EMERGENCY 必须能覆盖一切（包括安全层的限幅结果）。

---

## 4. 场景 C 演示（`scripts\run_demo.bat`）

### 试验 1：阶跃跟随与倒送抑制（无延迟）

```
  判定：无延迟=成立  无振荡=成立
  说明：阶跃瞬间的暂态倒送源于 PCS 物理惯性（0.2s 死区 + τ=0.5s），
        稳态段关口功率已稳定在 0 附近，防逆流约束生效。
```

### 试验 2：并网点边界抖动治理三档对照（无振荡）

激励：净负荷恒为 0，量测噪声 `σ = 3 kW`（关口功率 `σ ≈ 4.2 kW`）。

| 配置 | 指令总行程 | 方向反转 | 大幅跳变 | 无振荡判定 |
| --- | --- | --- | --- | --- |
| ① 基线（无滤波/无死区） | 4548 kW | 809 次 | 266 次 | 不成立 |
| ② + 量测滤波（α=0.05） | 3671 kW（−19%） | 689 次（−14%） | 199 次（−25%） | 不成立 |
| ③ + 量测滤波 + 死区滞环 | 1326 kW（**−70%**） | 183 次（**−77%**） | 185 次（−30%） | **成立** |

结论：**根因在量测侧**（安全边界的基准量 `base = P_load − P_pv` 带噪声），
量测低通滤波是主药，输出死区+滞环是辅药。

### 试验 3：变化率对照（工程折中）

| 变化率 | 最大倒送 | 倒送时长 | 安全裁剪 |
| --- | --- | --- | --- |
| 50 kW/s（安全层限） | 148.0 kW | 2.8 s | 1 次 |
| 2000 kW/s（物理层限） | 148.0 kW | 2.9 s | 0 次 |

结论：峰值倒送由**光伏阶跃瞬间的实际出力**决定，与变化率无关；
根治手段是光伏逆变器限功率或提前预留充电裕量。

---

## 5. 单元测试覆盖（T11~T16）

| 用例 | 覆盖点 |
| --- | --- |
| T11 | 输出门控不变量（闭环口径：非运行态恒 0） |
| T12 | 电表通信异常走 HOLD_LAST，不进 FAULT |
| T13 | 被控对象动态（死区/惯性/变化率/效率/SOC） |
| T14 | `OutputShaper` 死区清零 / 滞环 / 关闭时透传 |
| T15 | `LoopMetrics` 行程/反转/翻转/倒送统计 |
| T16 | 闭环端到端 —— 指令跟随、无倒送、安全兜底 |

---

## 6. 验收指标口径（`LoopMetrics`）

| 指标 | 口径 | 判定阈值 |
| --- | --- | --- |
| `mean_err` / `rmse` | `P_actual − P_cmd` 的均值 / 均方根 | — |
| `steady_err` | 末段 10% 的平均绝对误差 | `≤ 15 kW`（`no_lag`） |
| `over_delivery` | `Σmax(0, \|P_actual\| − \|P_cmd\|) / Σ\|P_cmd\|` | `≤ 25%`（`no_lag`） |
| `sign_flips` | 指令穿越零点次数（0.5 kW 死区） | `≤ 0.20/s`（`no_oscillation`） |
| `cmd_reversals` | 指令增量符号反转次数（`dir_eps = 2 kW`） | `≤ 1.00/s`（`no_oscillation`） |
| `cmd_travel` | `Σ\|Δcmd\|`，输出"抖动总量"的标准度量 | — |
| `max_reverse` / `reverse_duration` | 有效倒送（`P_grid < −5 kW`）的峰值 / 累计时长 | — |

> **为什么用 `cmd_travel`**：`cmd_reversals` 会把"死区造成的 bang-bang 输出"也计入反转，
> 无法反映死区的收益；`Σ|Δcmd|` 才是衡量 PCS 无效往复动作的正确口径。

---

## 7. 设计文档

闭环时序、第 ⑨ 步顺序论证、`OutputShaper` 语义、被控对象建模顺序、指标口径、
抖动三层治理的完整设计说明见 [`docs/design.md`](./design.md)。
