# BMS 项目长期记忆

## 工程结构约定

- **模块编号沿用时间线**：01 = B 组优化、02 = A 组安全、03 = 三实时控制器（anti_reverse_controller / pv_smoothing_controller / demand_management_controller + integration + shared）、04 = 策略管理层+仲裁器、
  **05 = 周期 5 统一安全约束引擎 / 06 = 周期 6 EMS 状态机 / 07 = 周期 7 实时控制闭环 / 08 = 周期 8 优化调度与实时控制协同**
  （2026-09-12 把原单体 `05/` 按周期粒度拆开，编号与周期一一对应）。
  **下一个可用编号是 `09`**（周期 11 系统联调 / 周期 12 最终验收）。不要打破编号，不要复用旧号。
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

### 周期 5~8 分层管控（05/ 模块，2026-09-12）

- **安全引擎（周期 5）是"系统级统一器"，不是设备保护**：`02/` 是设备侧安全处理器，`05/`
  在其之上把所有安全源（BMS 禁充放 / BMS 降功率 / SOC 上下限 / PCS 限功率 / 变压器容量 /
  温度 / 变化率 / 并网约束）统一折叠成**一个** `(p_lower, p_upper)` 区间，再作为 L0/L1 的
  `StrategyResult` 喂给 `04/` 仲裁器。
- **变化率（ramp）不是区间约束**：它是"相对上一条已下发指令的邻域限制"，只能做**后处理
  限速器**。若硬塞进区间求交，冷启动会锁死，并产生假的 `interval_contradiction`。
- **区间矛盾 ≠ 紧急**：满充 + 光伏大发 + 不许倒送 → 本拍无可行非零功率 → 应映射
  **DERATED（可自恢复）**，绝不能锁存 EMERGENCY（否则电池被冻结到人工复位）。
- **`ConstraintResult` 三个正交标志**：`active`（真的收紧了区间）/ `counts_as_derate`
  （驱动 DERATED）/ `binds_interval`（参与求交；ramp = false）。
- **死区/滞环必须放在输出级**（L2 纠偏是在仲裁之后叠加的）；且**上游必须有量测滤波**——
  否则安全层 `apply()` 会把指令钳到一个**带噪声**的边界上，直接覆盖掉下游死区。
- **抖动指标用总行程 `Σ|Δcmd|`**，不要用方向反转次数（死区输出是 bang-bang，反转次数会误判）。
- **滚动优化**：每 15 min 从实测 SOC 重新规划，且每次都规划完整 24 h（96 槽），预测按日周期
  回绕。实时纠偏 3 项 = SOC 偏差反馈（P+I，带抗饱和）+ 负荷偏差前馈 + 窗口电量预算补偿，
  统一被 `l2_correction_max_kw` 裁剪。
- **L2 `p_desired` 语义不统一**：防逆流 / 需量管理是**绝对目标**（转成地板/天花板），
  光伏平抑是**增量**（直接相加）。`merge_realtime_correction` 必须区分处理。
- **回退计划器要预留光伏余电裕度**：凌晨就充到 `soc_max` → 中午光伏无处可去 → 被迫倒送。
  用 `surplus_ahead[]` 反推每个时刻的 `soc_cap_at(k)`。

## 项目相关偏好与坑

- 用户偏好 C++17 + MinGW-w64 + 单文件可编译，Header-only 实现多于 .cpp 类拆分。
- gtest 不强依赖——`tests/` 用项目自带 `EXPECT_*` 宏（见 `03/防逆流控制器/tests/` 和 `04/tests/`）。
- bat 脚本直接 g++ 调用，避免中文路径在 git-bash 下编码问题。
- **bat 脚本必须 CRLF + 防"吞 CR"**（两个都会**静默失败，退出码仍是 0**）：
  1. LF 行尾 → `cmd` 解析出 `'曟搸' 不是内部或外部命令`。写完转 CRLF。
  2. **UTF-8 被 CP936 读取时，若某行末尾落在双字节字符的前导字节（0x81–0xFE）上，
     该字节会与行尾 `\r` 配成一对被吃掉 → 下一行的 `REM`/`echo` 前缀被吞，整行中文被当命令执行。**
     实测：65 字节（奇数）的中文标题行 + 下一行中文 REM → 报错；标题行加 1 个空格（66 字节）→ 干净。
     修法：模拟 CP936 扫描（`i += 1 if (c<0x80 or c==0x80 or c>0xFE) else 2`），
     越界（`i > n`）的行末尾补一个空格。纯 ASCII 行不受影响。
     完整脚本与说明见 skill `bms-ems-module`。
- **跨模块 include 是扁平的**（`#include "xxx.h"` 不带路径），靠 `-I` 搜索路径解析。
  因此**搬头文件到别的模块不用改任何 include**，只需给该模块配好 `-I`。
  各模块 `-I`：`05: src ../04/src (+../06/src 仅测试)`、`06: src ../04/src ../05/src`、
  `07: src ../04/src ../05/src ../06/src ../08/src`、`08: src ../04/src ../05/src ../06/src ../07/src`。
  头文件层无环；`07/` 与 `08/` 互为运行时调用关系。
- `code/` 目录是 Python 并行实现，不要碰、不要在重构里引用。

## 周期完成状态（V2.0 12 周计划）

- ✅ 1 梳理 / 2 接口 / 10 仿真（部分）
- ✅ **3 策略管理器** + **4 策略仲裁器**（2026-09-06 完成，含 17 单测 63 断言全过）
- ✅ **9 多策略组合 7 场景测试**（2026-09-06 完成，17 单测 63 断言全过，含 §7 全部场景）
- ✅ **5 安全约束引擎** + **6 EMS 状态机** + **7 实时控制闭环** + **8 优化调度协同**
  （2026-09-12 完成，落在新模块 **`05/`**，20 单测 **PASS=6594 FAIL=0**；
  根 `scripts/build_all.bat` 已扩到 **11 步** → `[BUILD ALL OK] All 11 components built`）
- ❌ 11 系统联调 / 12 最终验收

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
