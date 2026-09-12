# 05/ —— 周期 5：统一安全约束引擎

> 本模块是 [`工商业储能EMS调控策略设计方案.md`](../工商业储能EMS调控策略设计方案.md) §7 **周期 5** 的落地实现，
> 在 `02/`（设备侧安全处理器）与 `04/`（策略管理层 + 仲裁器）之上，把**全部安全约束统一折叠成一个功率区间**。

| | |
| --- | --- |
| 对应周期 | 周期 5：安全约束引擎 |
| 周期目标 | 「实现所有策略指令无法突破设备及系统安全边界。」 |
| 形态 | **纯头文件 C++17 库**（`src/safety_engine.h`），无外部依赖；`src/main.cpp` 为单场景演示 |
| 依赖 | `04/src`（`data_models.h` / `strategy_base.h`）；测试另需 `06/src`（见下） |
| 单元测试 | `tests/test_safety_engine.cpp`，**T01~T06 / 66 断言，全过** |
| 演示 | `src/main.cpp` → 场景 A：9 类约束逐条触发对照表 |
| 关键常量 | `dt = 100 ms`；`grid_filter_alpha` 默认 `1.0`（不滤波） |

---

## 1. 快速开始

```bat
cd 05
scripts\build.bat          :: 编 test_safety_engine.exe + safety_demo.exe
scripts\build_test.bat     :: 编 + 跑 T01~T06（应输出 PASS=66 FAIL=0 / ALL TESTS PASSED）
scripts\run_demo.bat       :: 跑场景 A，输出 → build\demo_output.txt
```

> **编译依赖**：头文件搜索路径为 `src` + `../04/src` + `../06/src`。
> 最后一个是因为 `tests/test_safety_engine.cpp` 的 T03 末尾用 `06/` 的状态机交叉验证
> 「区间矛盾 → DERATED 且**仍允许输出**」。头文件层面**无环**：`state_machine.h` →
> `safety_engine.h`（单向）。

---

## 2. 文件导览

```
05/
├── src/safety_engine.h            ← 9 类约束 → 统一区间 + 逐条 trace（本模块唯一头文件）
├── src/main.cpp                   ← 场景 A：19 个用例逐条触发对照
├── tests/test_safety_engine.cpp   ← T01~T06
├── docs/README.md                 ← 本文件
├── docs/design.md                 ← 设计说明（区间语义 / 三个正交标志位 / 变化率 / 矛盾语义 / 量测滤波）
├── scripts/{build,build_test,run_demo}.bat
└── build/                         ← 产物（git ignore）
```

---

## 3. 场景 A 演示（`scripts\run_demo.bat`）

19 个用例逐条验证 9 类约束的**区间语义**与**收敛结果**：

```
④ BMS 禁止充放       L0 禁闭  [0.0, 0.0]       bms_forbid         l0_safety
⑧ SOC 预警区（0.86）  降功率  [-100.0, 100.0]  soc_limit          derated
⑩ 电池温度故障（58℃） 紧急停机 [0.0, 0.0]       battery_temp       emergency
⑭ 并网不允许倒送      L1 限界  [-200.0, -200.0] grid_connect       l1_limited
⑰ 电网电压越限（1.15pu）紧急停机 [0.0, 0.0]      grid_quality       emergency
⑱ 功率变化率（上拍 +100）L1 限界 [-200.0, 200.0] none               l1_limited
⑲ BMS 通信丢失        L0 禁闭  [0.0, 0.0]       bms_forbid         l0_safety

统计：19 个用例 → L0 禁闭 9 个、L1 降额 6 个、紧急停机 3 个、区间矛盾 0 个
```

### 9 类约束 → 收敛语义映射

| 约束 | 层级 | 区间语义 |
| --- | --- | --- |
| `bms_forbid` | L0 | 禁充 → `p_lower = 0`；禁放 → `p_upper = 0`；通信丢失 → `[0, 0]` |
| `soc_limit` | L0 | 触及下限禁放 / 触及上限禁充 / 预警区折减 50% |
| `battery_temp` | L0 | ≥55℃ 禁充放（紧急）；≥45℃ 折减 50% |
| `bms_derate` | L1 | `min(BMS 上送限值, PCS 额定)` |
| `pcs_limit` | L1 | PCS 额定充放幅度 × 折减系数 |
| `transformer_limit` | L1 | 轻度过载按余量限放；极端过载禁放 |
| `grid_connect` | L1 | `P_grid ≥ g_min → p_upper ≤ base − g_min`；`P_grid ≤ g_max → p_lower ≥ base − g_max` |
| `grid_quality` | L1 | 频率/电压越限 → `[0, 0]`（并网合规） |
| `ramp_rate` | L1 | `\|p_cmd − p_last\| ≤ ramp_kw_per_s × dt`（**后置限速器，不参与求交**） |

收敛方式与接口规范 §2.5 一致：逐层求交，`p_lower = max(所有下界)`、`p_upper = min(所有上界)`。

---

## 4. 单元测试覆盖（T01~T06）

| 用例 | 覆盖点 |
| --- | --- |
| T01 | 9 类约束逐条区间语义（禁充/禁放/SOC/温度/PCS/变压器/并网） |
| T02 | 多约束区间求交 + binding trace（谁在限） |
| T03 | 区间矛盾 → `[0,0]` + DERATED（**不得**判为紧急） |
| T04 | 变化率走 slew limiter 后置限速，不参与求交 → 无假矛盾 |
| T05 | 并网约束区间推导（`base − g_min` / `base − g_max`） |
| T06 | 并网边界量测低通滤波（边界去抖） |

---

## 5. 与其他模块的关系

- **与 `02/`**：`02/` 是**设备侧**安全处理器（BMS 禁止充放 / 降功率 / 变压器过载）；
  `05/safety_engine.h` 在其之上做**系统级统一收敛**，把 9 条约束折叠成唯一 `(p_lower, p_upper)`，
  并保留逐条 trace 便于定位"谁在限"。
- **与 `04/`**：本模块**不自建仲裁器**，而是把安全结论折算成 L0/L1 层 `StrategyResult`
  喂给 `04/StrategyArbiter`，与 L2/L3 经济策略一起做区间收敛 —— 完全复用 §2.5 仲裁规则。
- **与 `06/`**：`safety_engine.h` 产出的 `SafetyVerdict::derated` / `emergency` 是状态机
  进入 DERATED / EMERGENCY 的判据（`06/src/state_machine.h` 单向依赖本模块）。
- **与 `07/`**：`SafetyEngine::apply()` 是闭环第 ⑨ 步的**安全兜底**，把最终指令压回安全区间。

---

## 6. 设计文档

约束语义、三个正交标志位、变化率取舍、区间矛盾语义、并网量测滤波的完整设计说明见
[`docs/design.md`](./design.md)。
