# 周期 1–4 达成度审计 — 摘要

详细报告：[`04/docs/audit-p1-p4.md`](./04/docs/audit-p1-p4.md)（含每条逐项核对、偏差清单、治理建议）

| 周期 | 简述 | 综合分 |
|---|---|---|
| **1 梳理** | 接口规范 v1.1（864 行）已就位；但**策略编号在 design / spec / 04/ 三处不一致**（design S01=BMS禁止, spec S01=峰谷, 04/ 跟随 design）；数据字典并入接口规范 §3，未独立成文 | **90%** |
| **2 统一接口** | 4 个统一数据模型已实现（Device/Realtime/StrategyResult/PowerCommand）；但 design §7 周期 2 列出 10 个字面字段（target_power / max_power / min_power / direction），04/ 改名为 p_desired / p_upper / p_lower，且 BatteryState / GridState 未独立成体 | **85%** |
| **3 管理器** | 注册/启停/状态/参数/生命周期 全功能 + L3 RunMode 独占 + 线程安全，全部达成 | **100%** |
| **4 仲裁器** | 5 步算法（区间收敛 → 同层加权 → desired_clip → 死区 → 滞环）全到位，17 用例 / 63 断言全过；**T07 典型场景用 pcs=200 而非 transformer=200 触发限功率**，未严格字面覆盖 design 经典例 | **95%** |
| **综合** | 周期 1+2 字面契约有缺口，周期 3+4 接近完美 | **92%** |

## 三个最值得优先治理的偏差

1. **🔴 策略编号 SSOT** —— 在 design / 接口规范 / 04 三处定义不统一，下游模块继续引用时会持续分叉
2. **🟡 field 重命名未回写 design** —— 04/ 实际是 p_upper 等价但更好；要么回写 design §3，要么在 design §7 周期 2 旁明确"按实施命名"
3. **🟡 T07 fixture 补正** —— 让变压器策略本身作为典型 200 kW 限功率源，验证"变压器过载也能被 L0 收紧"的链路

详细打分、各项证据代码引用、改进 ROI 排序见完整报告。

---

## 回写记录（2026-09-14）

上表「三个最值得优先治理的偏差」已全部关闭，处置方式如下（审计原文保持不动，
作为历史基线；本节记录后续回写，便于追溯"当初提的问题后来怎么解决的"）。

| # | 原偏差 | 处置 | 证据 |
|---|---|---|---|
| 1 | 🔴 策略编号 SSOT 三处不一致 | 明确 **04 为唯一真相源**，并在接口规范 §4.10.1 新增**权威映射表**（`S0x_…` ↔ 英文短名）；architecture.md「核心策略一~八」段首、design.md §3.3 均加交叉引用指向该表 | `docs/接口规范/EMS策略接口规范.md` §4.10.1；`docs/architecture.md` §七 前注；`04/docs/design.md` §3.3 |
| 2 | 🟡 field 重命名未回写 | 在需求原文 `工商业储能EMS调控策略设计方案.md` 周期 2 旁以「**实现口径（回写）**」块记录改名与理由（`max_power` 易被误读成"功率大小上限"，而本项目是**有符号区间**；符号约定 `P_bat>0` 放电）；原文一字未改 | `工商业储能EMS调控策略设计方案.md` 周期 2 |
| 3 | 🟡 T07 fixture 未覆盖 design 经典例 | T07 拆为两个子场景：**(a)** 上界来自 `pcs_rated_dis_kw=200`（原用例）；**(b)** 上界来自 `transformer_capacity_kw=200` 真实过载（`reason="tr_overload"`，区间 [10, 200]），直接断言变压器策略自身的输出 | `04/tests/test_arbiter.cpp` T07(b) |
| 4 | 🟡 BatteryState / GridState 未独立成体 | 在 `04/src/data_models.h` **显式成体**为只读视图（`BatteryState` / `GridState` + `battery_state_of()` / `grid_state_of()`）。**不替换** `RealtimeSnapshot` / `DeviceLimits` 的既有字段（它们是全项目公共契约），视图单向映射、不产生第二份真相；新增 T18 逐字段核对映射忠实性 | `04/src/data_models.h` §2.5；`04/tests/test_arbiter.cpp` T18 |

**测试规模变化**：`04/tests/test_arbiter.cpp` 由 17 用例 / 63 断言 → **18 用例 / 93 断言**（全绿）。

**仍然保留的偏差（有意为之，非缺口）**：
- `StrategyRequest` 未落地 —— 由 `evaluate(rt, dev)` 的形参列表取代（主动简化）。
- `direction` 字段取消 —— 方向由 `p_lower/p_upper` 的符号唯一确定（信息冗余）。
- `soc_life_planner`（接口规范 §4.8）无 04 实现 —— SOC 硬边界与预警降额归
  `05/` 安全约束引擎 `check_soc`；"按 SOH 修正区间"的寿命友好规划部分尚未落地。
