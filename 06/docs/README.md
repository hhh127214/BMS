# 06/ —— 周期 6：EMS 状态机

> 本模块是 [`工商业储能EMS调控策略设计方案.md`](../工商业储能EMS调控策略设计方案.md) §7 **周期 6** 的落地实现，
> 提供统一的 EMS 状态流转与**输出门控**，是"安全判定"与"功率下发"之间的**唯一闸门**。

| | |
| --- | --- |
| 对应周期 | 周期 6：EMS 状态机 |
| 周期目标 | 「实现系统全状态标准化管控。」 |
| 形态 | **纯头文件 C++17 库**（`src/state_machine.h`），无外部依赖；`src/main.cpp` 为单场景演示 |
| 依赖 | `05/src`（`safety_engine.h` 的 `SafetyVerdict`）+ `04/src`（`data_models.h`） |
| 单元测试 | `tests/test_state_machine.cpp`，**T07~T10 / 34 断言，全过** |
| 演示 | `src/main.cpp` → 场景 B：全状态流转 + 门控真值表 + SOE（**纯状态机口径，不依赖 07/**） |
| 关键常量 | `init_hold_s = 0.5`；`self_check_cycles = 5`；`derate_release_cycles = 10`；`fault_clear_cycles = 20` |

---

## 1. 快速开始

```bat
cd 06
scripts\build.bat          :: 编 test_state_machine.exe + fsm_demo.exe
scripts\build_test.bat     :: 编 + 跑 T07~T10（应输出 PASS=34 FAIL=0 / ALL TESTS PASSED）
scripts\run_demo.bat       :: 跑场景 B，输出 → build\demo_output.txt
```

> 本模块**不依赖 `07/`**：演示直接驱动 `EmsStateMachine`，不引入 `EmsRuntime`。
> 这样周期 6 的交付物可以独立验证，也让模块的编译依赖保持单向下行（06 → 05 → 04）。

---

## 2. 文件导览

```
06/
├── src/state_machine.h            ← 7 状态 / 8 类故障源 / 门控 / SOE / 急停锁存（本模块唯一头文件）
├── src/main.cpp                   ← 场景 B：9 个阶段 + 门控真值表 + SOE 迁移表
├── tests/test_state_machine.cpp   ← T07~T10
├── docs/README.md                 ← 本文件
├── docs/design.md                 ← 设计说明（状态迁移图 / 故障分类 / 门控不变量 / 安全行为）
├── scripts/{build,build_test,run_demo}.bat
└── build/                         ← 产物（git ignore）
```

---

## 3. 状态与迁移

```
INIT ──init_done──► SELF_CHECK ──selfcheck_pass──► READY ──start_command──► NORMAL
                      │                              ▲                        │
                      │ selfcheck_fail               │ fault_recovered        │ derate_enter
                      ▼                              │                        ▼
                    FAULT ◄──── fault_* ────────────┘                     DERATED
                      │                                                    │ derate_release
                      │ escalate_to_emergency                              └──► NORMAL
                      ▼
                 EMERGENCY ──manual_reset（需故障源已清除）──► INIT
```

7 状态：`INIT / SELF_CHECK / READY / NORMAL / DERATED / FAULT / EMERGENCY`。

---

## 4. 场景 B 演示（`scripts\run_demo.bat`）

9 个阶段 + 门控真值表 + SOE 迁移记录：

```
  t=   0.5s  SELF_CHECK 门控=禁止 INIT 停留 0.5s 结束
  t=   1.0s  READY      门控=禁止 自检连续通过 5 拍
  t=   1.1s  NORMAL     门控=允许 下发启动命令 run_request=true
  ...
  t=   2.6s  FAULT      门控=禁止 PCS 报故障
  t=   5.1s  READY      门控=禁止 故障清除满 20 拍
  t=   5.3s  EMERGENCY  门控=禁止 电池温度 58℃ 超故障阈值 55℃
  t=   8.3s  EMERGENCY  门控=禁止 温度恢复后等待 3s（不自愈）
  t=   9.5s  NORMAL     门控=允许 重新下发启动命令 → NORMAL

  状态      output_enabled strategies_enabled fail_safe
  --------------------------------------------------------------
  INIT        禁止         禁止             是
  SELF_CHECK  禁止         禁止             是
  READY       禁止         允许             否
  NORMAL      允许         允许             否
  DERATED     允许         允许             否
  FAULT       禁止         禁止             是
  EMERGENCY   禁止         禁止             是

  合计迁移 13 次；状态构造不变量破坏 = 0；
  FAULT/EMERGENCY 迁移次数 = 2（每次都应撤销运行许可，故障恢复后落 READY 待机）
```

> 门控真值表是**实测**的：演示对每种状态实际构造一台状态机推到该状态，再读取门控位，
> 而不是硬编码打印。

---

## 5. 单元测试覆盖（T07~T10）

| 用例 | 覆盖点 |
| --- | --- |
| T07 | INIT→SELF_CHECK→READY→NORMAL 标准启动 |
| T08 | DERATED 进入/解除（滞环） |
| T09 | FAULT 进入/恢复 + 运行许可撤销 |
| T10 | EMERGENCY 锁存 + 人工复位 |

> 「非运行态下发指令恒为 0」的**执行**验证在 `07/tests` T11（需要闭环链路才能观察功率指令）。

---

## 6. 与其他模块的关系

- **与 `05/`**：单向依赖。状态机的 DERATED / EMERGENCY 判据来自 `SafetyVerdict`
  （`derated` / `emergency`）。状态机**不参与功率计算**，只做状态判定与门控。
- **与 `07/`**：`EmsRuntime::step()` 的第 ④ 步调用本模块；第 ⑨ 步依据
  `output_enabled()` 强制把指令置 0（FAULT / EMERGENCY / INIT / SELF_CHECK / READY）。
- **与 `04/`**：`strategies_enabled()` 决定是否让 `04/StrategyManager` 出计划
  （READY 起允许评估，FAULT 以上停评）。

---

## 7. 设计文档

状态迁移图、8 类故障源分类、门控不变量、以及两个安全相关行为
（进 FAULT/EMERGENCY 撤销许可、EMERGENCY 锁存）的完整设计说明见
[`docs/design.md`](./design.md)。
