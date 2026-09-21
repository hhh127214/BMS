# STORY.md — 工商业储能 EMS 周期 1-4 交付汇报

## ① 用户意图对齐

- **目标受众**：项目内部评审/周汇报（用户=项目负责人 + 评审者）。场景是周例会 / 工程评审。
- **核心目标**：让评审 5 分钟内确认前 4 周对 V2.0 设计方案的落实度（4 个周期成果 + 测试覆盖 + 审计评分 + 治理差距 + 后续路线）。每一个结论都必须有数据/文件/测试支撑。
- **PPT 长度**：11 页（cover + catalog + 8 content + ending）。
- **视觉调性**（5 词锁死）：工程严谨 / 数据驱动 / 简洁克制 / 蓝色信任感 / 锚点对比。
- **内容边界**：
  - **必讲**：4 个周期的核心交付 + 5 步仲裁算法 + 测试覆盖指标 + 92% 审计 + 三大治理差距 + 后续 4 周路线图。
  - **可选**：方案源代码路径引用（如 `04/src/strategy_arbiter.h`）。
  - **不讲**：1/2/3 控制器内部细节（属于 V1.0 已交付物）；财务/商务/合同要素。
  - **禁碰**：纯营销话术、未发生的功能前瞻、空洞标题党。

## ② 页面布局骨架

### 页面总数与分章

不分章节扉页，单线 11 页；catalog 把 9 页 content 划成 4 个章节标签，章节扉页由"Period cards (P1→P4)"承担过渡。

| # | 页面 | 类型 | 角色 | 节奏 | 版式 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 01 | 封面 | cover | hero | peak | 全屏视觉+骑线文字 |
| 02 | 目录 + 12 周全景 | catalog | transition | transition | 左标题+右内容 |
| 03 | 周期 1：策略梳理 | content | supporting | valley | 左大图+右卡（N=1 大卡） |
| 04 | 周期 2：接口统一 | content | supporting | valley | 非对称双栏（65:35） |
| 05 | 周期 3：策略管理器 | content | supporting | valley | 左标题+右内容 |
| 06 | 周期 4：策略仲裁器 | content | hero | peak | 全幅图+骑线文字（5 步算法示意） |
| 07 | 测试覆盖 | content | hero | peak | 巨型数字+洞察（17 / 63 / 7） |
| 08 | 达成率审计 | content | supporting | valley | 非对称双栏（条形 + 表） |
| 09 | 三大治理差距 | content | supporting | valley | 上大图+下方卡片（3 卡） |
| 10 | 已交付物清单 + 后续路线 | content | supporting | valley | 数据密集型 + 双栏对比 |
| 11 | 总结与下一步 | ending | hero | peak | 居中金句/巨型数字 |

### 目录 ↔ 章节扉页契约

- 本 PPT 不设独立章节扉页（避免页面膨胀），将"章节元数据"嵌入 cover 后第一页（catalog），同时由各 Period 页（P1-P4）作 natural 过渡。
- catalog 用 4 行标签（P1 / P2 / P3 / P4 — 测试 — 审计 — 差距 — 路线 — 总结）承载分章契约，每行对应 1 个或多个 content 页。

### Hero 页定位（4/11 = 36%，合规）

- P01 封面（hero，默认）
- P06 周期 4 仲裁器 5 步算法（hero）
- P07 测试覆盖峰值（hero）
- P11 总结（hero，默认）

任何两 hero 之间至少间隔 1 supporting 页 ✅（P01→P03 间隔 1，P06→P07 连续 → **需要调整**）

调整：把 P06 改成 valley / supporting，把 5 步算法详情拆给 P07（hero）+ 用 P06 当 supporting 简述。结果：hero = P01 / P07 / P11 = 3 个，hero 占比 27%，舒服。同时 P06 用 `<上大图+下方卡片>`，把 5 步作为一个 card group 处理。

修正后骨架：
| # | 页面 | 类型 | 角色 | 节奏 | 版式 |
| :- | :--- | :--- | :--- | :--- | :--- |
| 01 | 封面 | cover | hero | peak | 全屏视觉+骑线文字 |
| 02 | 目录 + 12 周全景 | catalog | transition | transition | 左标题+右内容 |
| 03 | 周期 1：策略梳理 | content | supporting | valley | 上大图+下方卡片（9 策略矩阵） |
| 04 | 周期 2：接口统一 | content | supporting | valley | 非对称双栏 |
| 05 | 周期 3：策略管理器 | content | supporting | valley | 左标题+右内容（架构列表） |
| 06 | 周期 4：策略仲裁器（5 步） | content | supporting | valley | 上大图+下方卡片（5 步） |
| 07 | 测试覆盖 | content | hero | peak | 巨型数字+洞察 |
| 08 | 达成率审计 | content | supporting | valley | 非对称双栏（条形+表） |
| 09 | 三大治理差距 | content | valley | valley | 上大图+下方卡片（3 卡） |
| 10 | 已交付物清单 + 后续路线 | content | valley | valley | 数据密集型 + 双栏对比 |
| 11 | 总结与下一步 | ending | hero | peak | 居中金句 |

### rhythm 曲线
peak(P01) → transition(P02) → 4 个 valley(P03-P06) → peak(P07) → 3 个 valley(P08-P10) → peak(P11)

### 非对称版式预算

非对称版式出现页：P02 / P04 / P05 / P06 / P07 / P08 / P09 / P10 = 8 页，占比 8/11 ≈ 73%（远 ≥ 40%）。✅

### 对称版式预算

对称版式（`全屏视觉+骑线文字` P01 + `上大图+下方卡片` P03/P06/P09 + `居中金句` P11）= 5 页。但 P03/P06/P09 严格说也是非对称（上一栏 + 下一栏不等同）。我视 P01/P03/P06/P09/P11 为 anchor 视觉页（用 hero/hero-adjacent 的版式）。真正对称的是 P11 居中金句。合规。

## ③ 页面大纲

### P01 — 封面

| 字段 | 值 |
| :--- | :--- |
| title | 工商业储能 EMS · 周期 1-4 交付汇报 |
| type | cover |
| role | hero |
| rhythm | peak |
| layout | 全屏视觉+骑线文字 |
| visual | L1: hero_landscape.png（占左 60%）/ L3: 项目名水印 |
| visual_role | anchor |
| density | 字数约 40 / 图片 1 / 留白约 35% |
| anti_pattern | 禁止把项目名缩到 14px 小字；禁止全篇用单色铺满 |
| description | 周期 1-4 已完成：9 策略梳理 + 接口规范 v1.1 + 策略管理器 + 5 步仲裁器。17 用例 / 63 断言全绿，92% 达成率。 |

### P02 — 目录 + 12 周全景

| 字段 | 值 |
| :--- | :--- |
| title | 12 周路线与本次汇报范围 |
| type | catalog |
| role | transition |
| rhythm | transition |
| layout | 左标题+右内容 |
| visual | L1: 12 周甘特图（SVG）/ L3: 项目名 |
| visual_role | evidence |
| density | 字数约 200 / 图片 1 SVG / 留白约 20% |
| anti_pattern | 禁止把目录做成 9 个等宽卡片横排；禁止 4 行以下 |
| description | 12 周计划：周期 1/2/3/4/5/7 已完成，6/8/9/11/12 待办；本次汇报聚焦周期 1-4。 |

### P03 — 周期 1：策略梳理

| 字段 | 值 |
| :--- | :--- |
| title | 周期 1：9 大策略全景梳理 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 上大图+下方卡片（9 策略矩阵 3×3） |
| visual | L1: strategy_matrix.svg (3×3 矩阵) / L3: 编号 chip |
| visual_role | evidence |
| density | 字数约 220 / 图片 1 SVG / 留白约 20% |
| anti_pattern | 禁止把 9 个策略塞到一张表里塞满；禁止等宽卡片横排 |
| description | 9 策略按 L0-L3 分级：L0 BMS 禁止 / L1 BMS 降功率、变压器过载 / L2 需量、防逆流、光伏平抑 / L3 峰谷套利、动态优化、需求响应。**周期 1 核心交付：把分散在 01/02/03 三个组的实现统一梳理为 SSOT 列表。** |

### P04 — 周期 2：接口统一（接口规范 v1.1）

| 字段 | 值 |
| :--- | :--- |
| title | 周期 2：EMS 策略接口规范 v1.1 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 非对称双栏（左 65% 决策表 / 右 35% 卡片） |
| visual | L2: spec_cover.svg（规范封面缩略） / L3: v1.1 标签 |
| visual_role | evidence |
| density | 字数约 220 / 图片 1 SVG / 留白约 20% |
| anti_pattern | 禁止等宽四卡横排；禁止把 6 条决策混在一段文字里 |
| description | 关键决策 6 条：① P_bat 符号约定（放电为正）② 区间表示 `[p_lower, p_upper]` ③ desired_clip 触发条件 ④ DR 事件 5 字段 ⑤ ForecastOpt 输入格式 ⑥ 接口幂等 / 重入安全。**核心交付：docs/接口规范/EMS策略接口规范.md**。 |

### P05 — 周期 3：策略管理器（架构）

| 字段 | 值 |
| :--- | :--- |
| title | 周期 3：策略管理器架构 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 左标题+右内容 |
| visual | L1: arch_diagram.svg（4 文件 + 调用关系） / L3: 角色标签 |
| visual_role | evidence |
| density | 字数约 240 / 图片 1 SVG / 留白约 25% |
| anti_pattern | 禁止等宽多卡横排；禁止把 4 个文件名写进段落里 |
| description | 4 模块：data_models.h（功率区间 / DR 事件 / 仿真时钟）/ strategy_base.h（策略抽象）/ strategy_manager.h（注册 + tick 调度）/ strategies_9.h（9 策略实现）。**核心交付：单一 tick 入口 + 9 策略可插拔注册。** |

### P06 — 周期 4：策略仲裁器（5 步算法）

| 字段 | 值 |
| :--- | :--- |
| title | 周期 4：策略仲裁器 5 步收敛 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 上大图+下方 5 卡（5 步垂直条） |
| visual | L1: arbiter_5steps.svg（流程图） / L3: L0-L3 颜色标签 |
| visual_role | evidence |
| density | 字数约 240 / 图片 1 SVG / 留白约 20% |
| anti_pattern | 禁止把 5 步塞进一个段落；禁止对称等宽卡片 |
| description | 5 步：① 逐层区间收敛（L0→L3 取交） ② 同层加权平均 ③ 跨层不平均 ④ desired_clip（超出区间时夹紧） ⑤ 死区+滞回。**核心交付：04/src/strategy_arbiter.h，纯头文件实现。** |

### P07 — 测试覆盖（峰值页）

| 字段 | 值 |
| :--- | :--- |
| title | 测试覆盖：17 用例 / 63 断言 / 7 组合场景 |
| type | content |
| role | hero |
| rhythm | peak |
| layout | 巨型数字+洞察 |
| visual | L1: test_pyramid.svg / L3: PASS 字样 |
| visual_role | anchor |
| density | 字数约 100 / 3 张巨型数字卡 / 留白约 40% |
| anti_pattern | 禁止等宽卡片横排；禁止把巨型数字缩到 < 48px |
| description | 17 个单元测试覆盖：单策略回归 8 + L0/L1 边界 2 + 7 大组合场景 + 全部 9 策略同时启用 1。**失败 0 跳过 0；07/test_arbiter.exe 单测 100% 绿**。 |

### P08 — 达成率审计

| 字段 | 值 |
| :--- | :--- |
| title | 周期 1-4 审计：92% 达成率 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 非对称双栏（左条形图 / 右评分表） |
| visual | L1: audit_bars.svg / L3: 92% 巨数字 |
| visual_role | evidence |
| density | 字数约 200 / 图片 1 SVG / 留白约 22% |
| anti_pattern | 禁止用纯文字代替条形；禁止把 4 周期评分塞在一段文字里 |
| description | 各周期：P1=90%, P2=85%, P3=100%, P4=95%。**整体 92%**；丢分项：SSOT 未建、字段命名 split、T07 fixture 偏差。完整报告见 `04/docs/audit-p1-p4.md`。 |

### P09 — 三大治理差距

| 字段 | 值 |
| :--- | :--- |
| title | 三大治理差距（已识别，待修） |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 上大图+下方 3 卡（3 大差距） |
| visual | L1: governance_gaps.svg / L3: gap 标号 |
| visual_role | evidence |
| density | 字数约 220 / 3 卡各 60+ / 留白约 25% |
| anti_pattern | 禁止 3 卡完全等宽无差异；禁止把差距描述写成"失败清单"语气 |
| description | (a) **SSOT**：策略 ID 命名 3 套并存（design §3 / spec §4.10 / 04/strategy_id），需统一；(b) **字段命名**：design §7 用 target_power/max_power/min_power，04/ 用 p_desired/p_upper/p_lower；(c) **T07 fixture**：误用 PCS 容量 200 而非变压器 200 作为限制源。每项均 ≤ 1 工时可修。 |

### P10 — 已交付物清单 + 后续路线

| 字段 | 值 |
| :--- | :--- |
| title | 已交付物清单 + 后续路线 |
| type | content |
| role | supporting |
| rhythm | valley |
| layout | 非对称双栏（左已交付目录 / 右路线甘特） |
| visual | L2: roadmap_remaining.svg / L3: 状态标 |
| visual_role | evidence |
| density | 字数约 260 / 图片 1 SVG / 留白约 18% |
| anti_pattern | 禁止把已交付与待办混在一列；禁止路线图无明确时间线 |
| description | 已交付：04/src/*（6 文件）+ tests + docs/audit + 工商业储能EMS调控策略设计方案.md。**后续 4 个周期**：周期 6（状态机）+ 周期 8（01→04 接驳）+ 周期 11（端到端联调）+ 周期 12（最终验收）。 |

### P11 — 总结与下一步（hero）

| 字段 | 值 |
| :--- | :--- |
| title | 92% 按时 / 5 步算法可信 / 差距已记账 |
| type | ending |
| role | hero |
| rhythm | peak |
| layout | 居中金句/巨型数字 |
| visual | L3: 项目徽标 + 数据标注（17/63/92%/9 策略） |
| visual_role | anchor |
| density | 字数约 60 / 巨型数字 4 个 / 留白约 50% |
| anti_pattern | 禁止把结尾做成"谢谢观看"卡片；禁止巨型数字 < 48px |
| description | **核心金句**："L0 安全 / L1 局部约束 / L2 经济 / L3 全局优化，分层收敛，单 tick 闭环。"下一步：3 个治理差距先修完，再启动周期 6 状态机。 |

## 数据落点（必须接"所以呢"）

- 17/63 测试：100% 绿——**意味着仲裁器在组合场景下的边界值已被显式覆盖**，不是"通过了"的空话。
- 92% 达成率：**意味着 8% 的差距已经被审计识别**，是可控可修的，而非隐藏债务。
- 5 步算法：**意味着仲裁不再是"策略合并 + 兜底"的暗箱**，而是可预测、可单测的白盒。
- 9 策略 × L0-L3：**意味着任何新策略只需注册，就能被自动仲裁**，未来扩展成本接近 0。
