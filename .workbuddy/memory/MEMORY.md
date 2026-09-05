# BMS 项目长期记忆

## 工程结构约定

- **模块编号沿用时间线**：01 = B 组优化、02 = A 组安全、03 = 三实时控制器（anti_reverse_controller / pv_smoothing_controller / demand_management_controller + integration + shared）、04 = 策略管理层+仲裁器。
  新增模块请用 05+ 或子目录扩展，不要打破编号。
- **03/ 子目录统一英文**（2026-09-06 重组完成，旧中文名已 `git mv` 改名；git log + CHANGES.md §10 留有变更记录）
- **每个模块统一结构**：`src/` `tests/` `data/` `samples/` `docs/` `scripts/build.bat` `build/`（产物）；图表汇总用 `figs/`
- **统一 Windows 入口**：根 `scripts\build_all.bat` 一键编全部，新模块必须在其中挂上分步号。
- **不在根 README 里写实现细节**——只放"是什么 / 在哪 / 怎么跑 / 设计在哪"。详细设计下沉到模块 docs/。
- **`code/` 目录是 Python 并行实现** —— .gitignore 排除 + 不要碰、不要在重构里引用。

## 关键技术决策

### L0-L3 多策略仲裁模式（04/strategy_arbiter.h）

适用于任何"多个并行决策者同时给一个执行器出指令"的场景：
1. 每个决策者产出 (p_lower, p_upper, desired, weight)，分别表征"我能接受的下限/上限/我的目标值/我的可信度"
2. 按优先级分层（L0>L1>L2>L3），层内取 **max(p_lower)** + **min(p_upper)** 做区间收敛
3. 跨层不平均——上层只能"收紧"下层区间，不能"拉宽"
4. 仅在最低层（L3）做 desired 的同层**加权平均**
5. 把算出来的 desired `desired_clip` 到收敛区间，标记 `clamped=true`，写明 reason
6. 死区+滞环切换 On/Off 模式，避免抖动

参考实现：`C:\Users\17128\Desktop\BMS\04\src\strategy_arbiter.h`，完整 10 个单测覆盖。

### Power 符号约定（项目统一）

- P_bat：放电为正，充电为负
- P_grid：外购为正，倒送为负
- P_load：负荷（>0）
- P_pv：光伏出力（>0）

写新策略时务必遵从这套约定；`dis_forbidden → p_upper=0`、`chg_forbidden → p_lower=0`。

## 项目相关偏好与坑

- 用户偏好 C++17 + MinGW-w64 + 单文件可编译，Header-only 实现多于 .cpp 类拆分。
- gtest 不强依赖——`tests/` 用项目自带 `EXPECT_*` 宏（见 `03/防逆流控制器/tests/` 和 `04/tests/`）。
- bat 脚本直接 g++ 调用，避免中文路径在 git-bash 下编码问题。
- `code/` 目录是 Python 并行实现，不要碰、不要在重构里引用。

## 周期完成状态（V2.0 12 周计划）

- ✅ 1 梳理 / 2 接口 / 5 安全 / 7 实时闭环（部分）/ 10 仿真（部分）
- ✅ **3 策略管理器** + **4 策略仲裁器**（2026-09-06 完成，含 17 单测 63 断言全过）
- ✅ **9 多策略组合 7 场景测试**（2026-09-06 完成，17 单测 63 断言全过，含 §7 全部场景）
- ❌ 6 状态机 / 8 优化协同 / 11 系统联调 / 12 最终验收

### 周期 1–4 字面 vs 功能达成度（审计于 2026-09-06）

| 周期 | 字面 | 功能 | 主要偏差 |
|---|---|---|---|
| 1 梳理 | 90% | 100% | 数据字典并入接口规范未独立；策略编号三方不齐 |
| 2 统一接口 | 80% | 95% | field rename（p_desired 等）未回写 design §7 周期 2 字面 |
| 3 管理器 | 100% | 100% | — |
| 4 仲裁器 | 95% | 100% | T07 用 pcs=200 而非 transformer=200 触发限功率源 |

**综合 92%**。详细审计：`04/docs/audit-p1-p4.md`；摘要 `04/docs/audit-p1-p4-summary.md`。

## 已知治理缺口（不动代码级别）

1. **SSOT 文档缺位**：策略编号在 design §3 / spec §4.10 / 04 `strategy_id` 三处定义不同
2. **field rename 回写 design**：04/ 实际命名更紧凑（`p_desired/p_upper/p_lower`），但 design §7 周期 2 字面要求 `target_power/max_power/min_power`
3. **T07 fixture**：design 经典例用 `transformer=200`，T07 用 `pcs=200`
4. **BatteryState/GridState 未独立成体**：现折合到 DeviceLimits/RealtimeSnapshot 字段

## 测试隔离经验

多策略组合测试时，**默认 fixture 容易"误触发"非目标策略**：

| 干扰源 | 默认值 | 触发条件 | 隔离方法 |
|---|---|---|---|
| Transformer 极端过载 | capacity=250 | ratio = (\|P_grid\| + 0.1·P_load)/cap > 1.10 → 禁放 (p_upper=0) | 把 capacity 设为 1000+ |
| BMS 限功率 | bms_dis_limit_kw=100 | 触发 L1 p_upper ≤ 100 | 视场景而定：需要测就调小，不需要测就调大 |
| PeakValley 强烈输出 | 默认 | TouType::kPeak/SHARP → 强 discharge 期望 | 视场景调整 tou 或 set_param("P_discharge", 0) |

DR 注入事件：本地持有 `shared_ptr<DemandResponseStrategy>` → `set_event` → `register`。  
DrEvent 字段：`active`、`target_kw`、`end_ts`（不是 "peak_window_s"）。
