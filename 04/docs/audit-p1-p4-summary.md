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
