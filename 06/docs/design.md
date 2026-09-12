# 06/ 设计说明 —— 周期 6：EMS 状态机

> 阅读前建议先看 [`../../docs/接口规范/EMS策略接口规范.md`](../../docs/接口规范/EMS策略接口规范.md) §2.5 / §5 / §6 与 [`../../工商业储能EMS调控策略设计方案.md`](../../工商业储能EMS调控策略设计方案.md) §7。

---

## 0. 全系统符号约定

| 符号 | 约定 |
| --- | --- |
| `P_bat` | **放电为正、充电为负** |
| `P_grid` | **取电为正、倒送（馈网）为负** |
| `P_load` | 恒 `> 0`（本地负荷） |
| `P_pv` | 恒 `> 0`（光伏发电） |
| 功率平衡 | `P_grid = P_load − P_pv − P_bat` |

由平衡式可把"并网约束"直接翻译成对 `P_bat` 的区间：

```
P_grid ≥ g_min  →  P_bat ≤ (P_load − P_pv) − g_min      （上界）
P_grid ≤ g_max  →  P_bat ≥ (P_load − P_pv) − g_max      （下界）
```

---

## 2. 周期 6：EMS 状态机（`state_machine.h`）

> 设计方案原文："实现系统全状态标准化管控。"

### 2.1 状态与迁移

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

### 2.2 8 类故障源（`FaultFlags`）

`bms_comm_lost` / `pcs_comm_lost` / `meter_comm_lost` / `pcs_fault` / `data_invalid` / `device_offline` / `temp_fault` / `emergency_stop`

分两类处理：

```cpp
bool emergency() const { return emergency_stop || temp_fault; }        // → EMERGENCY（锁存）
bool fault() const {                                                     // → FAULT（可恢复）
    return bms_comm_lost || pcs_comm_lost || pcs_fault ||
           data_invalid || device_offline;
}
```

> **`meter_comm_lost` 刻意不进 `fault()`** —— 按接口规范 §6，电表通信异常走 **HOLD_LAST** 降级（复用上一拍指令），而非停机。

### 2.3 输出门控不变量

```cpp
bool output_enabled() const {          // 仅 NORMAL / DERATED（READY 需显式放开）
    if (state_ == EmsState::kNormal || state_ == EmsState::kDerated) return true;
    if (state_ == EmsState::kReady) return cfg_.allow_ready_output;
    return false;
}
bool strategies_enabled() const { ... }  // READY 起允许评估（可出计划，不可出功率）
```

**不变量**：非运行态（INIT / SELF_CHECK / READY / FAULT / EMERGENCY）下发的指令恒为 0。
场景 B 与 T11 均对此做了断言校验（实测违规 0 次）。

### 2.4 安全相关的两个行为

1. **进入 FAULT / EMERGENCY 时撤销运行许可**：`run_request_ = false`，故障恢复后停在 READY，必须**重新下发启动命令**，避免故障未排除就自动带载。
2. **EMERGENCY 锁存**：只有显式 `reset_emergency()` 才能离开，且仍存在故障源时**拒绝复位**（返回 false）。
3. **矛盾不升级**：见 §1.5 —— 矛盾只进 DERATED。

---

## 关键不变量

| 不变量 | 保障位置 | 校验 |
| --- | --- | --- |
| 进入 FAULT/EMERGENCY 撤销运行许可 | `EmsStateMachine::transit()` | `06/tests` T09、T10 |
| EMERGENCY 锁存且故障未清时拒绝复位 | `reset_emergency()` | `06/tests` T10 |
| 全部迁移带滞环/计数去抖 | `derate_release_cycles` / `fault_clear_cycles` | `06/tests` T08、T09 |
| 非运行态下发指令恒为 0 | `output_enabled()` + `07/` 第 ⑨ 步门控 | `07/tests` T11、`06/` 演示真值表 |


---

## 已知边界与后续工作

- **门控的"执行"验证在 `07/`**：本模块只负责判定 `output_enabled()`；把该判定真正施加到功率指令上属闭环链路（`07/src/realtime_loop.h` 第 ⑨ 步），故不变量断言在 `07/tests` T11。
- **状态数扩展**：若后续要区分"并网/离网"或"黑启动"态，建议新增枚举值而不是复用 DERATED，以免污染"降额"的语义与门控真值表。
- **SOE 未持久化**：`history()` 只在内存中保留，长期运行需要外送 SOE 服务或落盘。

