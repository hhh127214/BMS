# 工商业储能 EMS V2.0 最终验收报告

- 验收依据：《工商业储能EMS调控策略设计方案》§7 周期 12
- 生成时间：2026-09-20 21:26:07
- 结论：**通过**（55 / 55 项通过，0 个维度未全通过）

## 维度总览

| 维度 | 名称 | 通过 | 合计 | 结论 |
|---|---|---|---|---|
| A1 | 功能验收 | 9 | 9 | PASS |
| A2 | 策略验收 | 9 | 9 | PASS |
| A3 | 安全验收 | 11 | 11 | PASS |
| A4 | 性能验收 | 5 | 5 | PASS |
| A5 | 稳定性验收 | 7 | 7 | PASS |
| A6 | 多策略协同验收 | 8 | 8 | PASS |
| A7 | 文档验收 | 6 | 6 | PASS |
| **合计** | — | **55** | **55** | **PASS** |

## A1 功能验收

> 验收对象：策略注册/启停、全链路闭环、状态机七态可达、设备 I/O 三适配器等价、配置化往返、日计划装载、关口功率单一数据源（电表口径）

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A1-01 | 9 策略注册完整 | mgr.size()==9 且 9 个 strategy_id 全部可查到 | registered=9, id_hit=9/9 | PASS |
| A1-02 | 策略启停生效 | start_all() 后 tick 返回 9 条；stop_all() 后返回 0 条 | start_all→9 条, stop_all→0 条 | PASS |
| A1-03 | 全链路闭环可跑（2000 拍 @ 10 Hz） | log().size()==2000 且每拍 p_cmd 有限（非 NaN/Inf） | log_rows=2000, steps=2000, final_state=DERATED | PASS |
| A1-04 | 状态机七态全部可达 | INIT/SELF_CHECK/READY/NORMAL/DERATED/FAULT/EMERGENCY 七态各至少到过一次 | reached=7/7, 末态=EMERGENCY（锁存 ✔） | PASS |
| A1-05 | 设备 I/O 三适配器逐拍等价 | SimDeviceIO / MemoryDeviceIO / RtDbDeviceIO 在 200 拍同一指令序列下 P_bat 与 SOC 逐拍偏差 ≤ 1e-9 | max|Δ(Sim,Mem)|=0.000000000000, max|Δ(Sim,RT_DB)|=0.000000000000 | PASS |
| A1-06 | 40 点表自检（点名/单位/索引一致） | RtDbDeviceIO::self_check()==0 且 is_open()==true | self_check() rc=0, 点数=40, stale_reads=0, is_open=true | PASS |
| A1-07 | 配置化闭环（导出→解析→生效） | to_json → parse → apply_config 后 dt_s / BMS 限值 / SOC 上下限与配置一致 | parse=ok, apply_errors=0, json_bytes=2613, dt_s=0.100, bms_dis=180.00, soc=[0.120, 0.880] | PASS |
| A1-08 | 日计划装载与跟踪 | 96 点 DayPlan 装载后 plan_valid()==true，且 t=20 s 仍落在第 0 槽（谷时，plan_target ≈ -100 kW） | plan_valid=true, slots=96, t=20s 时 plan_target=-100.00 kW | PASS |
| A1-09 | 关口功率单一数据源（电表口径） | 电表读数刻意与三路推算差 40 kW：算法用的关口功率必须等于电表读数，且与记录路径同口径（反向守卫：两源确实不同，判据有区分度） | 电表=429.59 kW, 三路平衡=389.59 kW, 算法用=429.59 kW, 两路径差=0.000000 | PASS |

## A2 策略验收

> 验收对象：9 个策略的设计意图（设计方案 §3.1~§3.9）在触发工况下的单点核对

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A2-01 | S01 BMS 禁止充放（L0 硬禁闭） | 禁放→p_upper=0；禁充→p_lower=0；同时禁→区间收成 [0,0] | 禁放 upper=0.0, 禁充 lower=0.0, 双禁 [0.0, 0.0], reason=bms_forbid_discharge | PASS |
| A2-02 | S02 BMS 请求降功率（L1） | BMS 限值 100/150 且 PCS 250/250 → 区间 [-100, 150] 且 active | [-100.0, 150.0], bms_derating | PASS |
| A2-03 | S03 变压器过载限功率（L1） | 负载率 0.953>0.95 → 区间收紧（active）；容量=0 → 不施加约束 | 过载 [1.0, 250.0] tr_overload；无容量 [-1000000000000000000, 1000000000000000000] | PASS |
| A2-04 | S04 需量管理（L2） | 窗口预测均值 < 契约 → 不动作（desired=0）；> 契约 → 放电削峰（desired>0） | 未超限 desired=0.00 (demand_within)；超限 desired=166.67 (demand_active) | PASS |
| A2-05 | S05 防逆流（L2） | 关口 -80 kW（倒送）→ 指令充电 80 kW（p_desired=-80）；无倒送→不动作 | 倒送时 desired=-80.00 (anti_reverse_active)；正常时 active=false | PASS |
| A2-06 | S06 光伏出力平抑（L2） | 光伏突升 200 kW → 储能吸收（p_desired<0） | desired=-100.00 (smoothing_active) | PASS |
| A2-07 | S07 峰谷套利（L3 / Timed） | 谷→充（<0）、平→0、尖→放（>0）；尖峰+SOC 0.05→禁放、谷时+SOC 0.95→禁充（SOC 保护是方向性的） | 谷=-80.0, 平=0.0, 尖=80.0, 尖+低SOC=0.0 (peak_valley_soc_low), 谷+高SOC=0.0 (peak_valley_soc_high) | PASS |
| A2-08 | S08 动态预测优化（L3 / MPC） | 负荷 > 契约 → 放电（>0）；负荷 < 契约 → 充电（<0） | 400 kW→45.00, 100 kW→-45.00 | PASS |
| A2-09 | S09 需求响应（L3 / Custom） | 事件激活→按 target 出力 150 kW；事件过期 / 未下发事件→不动作 | 激活=150.0, 过期=0.0, 无事件=0.0 (dr_idle) | PASS |

## A3 安全验收

> 验收对象：9 条安全约束逐位注入、区间矛盾语义、急停锁存、全域硬不变量（p_cmd ∈ [p_lower, p_upper]）

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A3-01 | 9 条安全约束全部被评估 | verdict.items 含 bms_forbid / bms_derate / soc_limit / pcs_limit / transformer_limit / battery_temp / ramp_rate / grid_connect / grid_quality | items=9, name_hit=9/9 | PASS |
| A3-02 | BMS 禁充放（L0 硬禁闭） | 禁放→约束 p_upper=0 且 verdict 上界=0；禁充→下界=0；l0_hard=true | 禁放 verdict=[-250.0, 0.0] l0_hard=true；禁充 verdict=[0.0, 200.0] | PASS |
| A3-03 | SOC 上下限（L0：绝对限 + 预警降额） | SOC 0.08→禁放（上界 0，hard）；0.92→禁充（下界 0，hard）；0.14→降额（counts_as_derate → verdict.derated）；三种情形都不得升级为 emergency | 低 SOC 上界=0.0 hard=true, 高 SOC 下界=0.0, 预警 derate=true, 预警 verdict.derated=true, emergency=false | PASS |
| A3-04 | 电池温度（L0：预警降额 / 故障禁闭） | 48 ℃→active 且 counts_as_derate；60 ℃→禁闭且 emergency=true | 48℃ active=true derate=true；60℃ emergency=true reason=temp_fault | PASS |
| A3-05 | PCS 限功率（L1 折减系数） | 折减系数 0.5 × 额定 250 → 约束区间 [-125, 125] 且参与求交 | [-125.0, 125.0], binds=true | PASS |
| A3-06 | 变压器容量（L1，含极端过载投影） | 负载率超阈值 → 区间收紧且参与求交；极端过载 → 仍给出有限边界（不得放开为无穷区间） | 常规 [127.0, 250.0] tr_extreme_overload；极端 [250.0, 250.0] tr_extreme_sat_discharge | PASS |
| A3-07 | 变化率 = 后置限速器（不是区间约束） | 上一拍 20 kW 时 ramp_rate 约束 active=true、margin=20 kW（200 kW/s × 0.1 s），但 binds_interval=false 且 counts_as_derate=false（否则会与硬安全区间产生假矛盾） | active=true, binds=false, derate=false, margin=20.00 kW | PASS |
| A3-08 | 并网约束（不倒送，L1） | P_load=250, P_pv=50 → 上界 = 200 kW（保证 P_grid ≥ 0） | 约束上界=200.00, verdict 上界=200.00 kW | PASS |
| A3-09 | 区间矛盾 → DERATED（可自恢复），绝不锁存 EMERGENCY | 满充禁充 ∩ 不许倒送 → contradiction=true、derated=true、emergency=false、区间收成 [0,0] | contradiction=true, derated=true, emergency=false, 区间=[0.0, 0.0], reason=interval_contradiction | PASS |
| A3-10 | 急停锁存 + 显式复位 | 急停后 205 拍仍停在 EMERGENCY（锁存）；reset_emergency() 后才离开 | NORMAL → EMERGENCY → 205 拍后仍 EMERGENCY → reset → SELF_CHECK | PASS |
| A3-11 | 全域硬不变量：p_cmd ∈ [p_lower, p_upper] | 7 场景抽样（各 2000 拍）：指令逃逸 + 功率超限 + 门控失效 合计 = 0 | 抽样拍数=14000, 越界合计=0, 场景通过=7/7 | PASS |

## A4 性能验收

> 验收对象：单拍闭环耗时（均值/峰值/占用率）、24h 离线仿真墙钟加速比、长跑进程资源无泄漏

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A4-01 | 单拍闭环耗时（均值） | mean_cycle_us ≤ 1000 us（10 Hz 控制周期下即 CPU 占用 ≤ 1.00%） | 9.50 us / 20000 拍 | PASS |
| A4-02 | 单拍闭环耗时（峰值，含首拍冷启动） | max_cycle_us ≤ 50000 us（10 Hz 控制周期的一半：单拍不得吃掉半个节拍） | 375.80 us | PASS |
| A4-03 | 单拍时间占用率 | mean_cycle_us / (dt_s × 1e6) ≤ 20.0% | 0.010% (dt=0.100 s) | PASS |
| A4-04 | 长跑进程资源无泄漏 | 20000 拍前后 GetProcessHandleCount 增量 ≤ 0 且 工作集增量 ≤ 64 MB | handle Δ=0, RSS Δ=4.01 MB | PASS |
| A4-05 | 24h 离线仿真墙钟加速比 | 24 h（86400 s）仿真墙钟 ≤ 43.2 s（即加速比 ≥ 2000×） | wall=0.97 s, speedup=89149×, steps=86400 | PASS |

## A5 稳定性验收

> 验收对象：24 h 长稳（10/ 全栈 + 三段故障窗）、跨进程闭环长稳（11/ RT_DB）、采集零 stale、SOE 零丢弃、状态迁移与指令抖动有界、可观测性与 log_every 解耦、BMS 禁充放位（设备侧上报 → 下发指令不放电）

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A5-01 | 24h 长稳：硬不变量逐拍成立 | 86400 拍（1 s 步长，含 3 段故障窗）内 指令逃逸 + 功率超限 + 门控失效 合计 = 0 | steps=86400, 越界=0, 故障窗拍数=1621, 告警=1646, 经济净收益=734 元 | PASS |
| A5-02 | 24h 长稳：状态机无抖动 | 状态迁移次数 ≤ 30 次（一天内不应反复来回切） | state_changes=17（86400 拍） | PASS |
| A5-03 | 24h 长稳：指令抖动有界 | 指令总行程速率 ≤ 5.0 kW/s，且 符号翻转率 ≤ 0.20 /s、方向反转率 ≤ 1.00 /s（07/ 工程口径） | travel=6822.2 kW (0.079 kW/s), flips=0.0006/s, rev=0.0012/s | PASS |
| A5-04 | 跨进程闭环长稳（RT_DB 共享内存） | 6000 拍：EMS 采集 stale_reads=0 且 设备侧观测到的指令越区间拍数=0 | ticks=6000, stale=0, 指令越 EMS 区间=0, SOE=8 | PASS |
| A5-05 | SOE 日志零丢弃 | 跨进程长稳结束后 soe().dropped()==0（容量足够 + 边沿检测正确） | ran=true, dropped=0, events=8 | PASS |
| A5-06 | 可观测性与 log_every 解耦 | log_every=1 与 =10 两种配置下观察者累计步数相同（均 = 实际拍数 2000） | log_every=1 → steps=2000 (SOE 4)；log_every=10 → steps=2000 (SOE 4) | PASS |
| A5-07 | BMS 禁放位：设备侧上报 → 下发指令不放电 | 禁放窗内 0 拍放电、且非门控拍的权限上界全部被收死（反向守卫：窗内 ≥ 5 拍非门控）；走设备侧写点 → 共享内存 → 每拍刷新 的真实路径 | 窗内非门控拍=10, 上界收死=9, 放电拍数=0 | PASS |

## A6 多策略协同验收

> 验收对象：设计方案 §7 周期 9 的 7 个重点验证场景（闭环时序级）全通过：杜绝指令冲突、互相覆盖、频繁切换、双向充放电、功率超限

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A6-01 | S1 峰谷套利 + BMS降功率 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0633/s, rev=0.0667/s, SOC=[0.498, 0.508] | PASS |
| A6-02 | S2 峰谷套利 + 变压器过载 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0650/s, rev=0.0983/s, SOC=[0.487, 0.501] | PASS |
| A6-03 | S3 需量管理 + 防逆流 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0000/s, rev=0.0083/s, SOC=[0.473, 0.500] | PASS |
| A6-04 | S4 光伏平抑 + 防逆流 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0250/s, rev=0.0417/s, SOC=[0.500, 0.540] | PASS |
| A6-05 | S5 动态优化 + 需量管理 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0183/s, rev=0.0517/s, SOC=[0.496, 0.512] | PASS |
| A6-06 | S6 需求响应 + 峰谷套利 + BMS限制 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0683/s, rev=0.0733/s, SOC=[0.498, 0.506] | PASS |
| A6-07 | S7 9策略全量同时启用 | 违规计数全 0（指令冲突/互相覆盖/频繁切换/双向充放/功率超限） | samples=12000, over_limit=0, out_of_interval=0, grid_breach=0, tr_breach=0, gated_nonzero=0, flip=0.0617/s, rev=0.0700/s, SOC=[0.495, 0.509] | PASS |
| A6-08 | 7 场景汇总 | 7/7 场景全部通过 | 通过 7/7 | PASS |

## A7 文档验收

> 验收对象：根文档、各模块 docs/README.md、各模块构建脚本、统一构建入口步数、根 README 模块索引、验收报告落盘能力

| 编号 | 检查项 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| A7-01 | 根文档齐备且非空 | README.md / CHANGES.md / 设计方案 三份均存在且 ≥ 200 字节 | README.md(74828 字节) CHANGES.md(204842 字节) 工商业储能EMS调控策略设计方案.md(32255 字节)  | PASS |
| A7-02 | 各模块 docs/README.md 齐备 | 17 个模块文档均存在且 ≥ 200 字节 | hit=17/17 | PASS |
| A7-03 | 各模块构建脚本齐备 | 每个模块的 scripts/ 下存在 build.bat 或 build_test.bat（≥ 50 字节） | hit=17/17 | PASS |
| A7-04 | 统一构建入口步数自洽且覆盖全部模块 | scripts/build_all.bat 的步骤号恰好是 1..N 无缺号（N ≥ 27，01..12 + P1/P2 各含 build 与 test 步）、14 个模块目录全部被引用、成功/失败横幅与 N 一致 | N=31, step_markers=31/31, module_refs=14/14, banner=ok | PASS |
| A7-05 | 根 README 是全项目索引 | README.md 的模块清单里出现 01/ .. 12/ 与 P1/ P2/ 全部 14 项 | hit=14/14 | PASS |
| A7-06 | 验收报告三件套落盘能力（md / json / html） | 渲染 + 写盘成功，三个文件均 ≥ 1 KB（正式报告由 12/scripts/run_acceptance.bat 写入 12/docs/） | ACCEPTANCE-REPORT.md=1736B ACCEPTANCE-REPORT.json=1803B ACCEPTANCE-REPORT.html=3737B  | PASS |

---

本报告由 `12/`（周期 12 最终验收模块）自动生成，全部检查项均可通过 `12/scripts/run_acceptance.bat` 复现。
