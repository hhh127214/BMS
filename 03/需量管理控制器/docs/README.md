# 需量管理控制器

通过储能放电控制关口需量（含电网供电功率）不超过 `D_target`（如 250 kW），避免需量电费罚款。
采用滑动窗口 PI 控制，对"窗口平均功率 - D_target"做无超调收敛。

| | |
|---|---|
| 入口 | `src/DemandController.{h,cpp}` |
| 算法 | 滑动窗口（`T_window`/`Ts`）+ PI + 遇限削弱积分 + dP_max 爬坡限速 |
| 控制周期 | 100 ms（`cfg.Ts`），窗口长度典型 900 s（9000 个样本） |
| 输入 | `P_grid`（实时关口功率）, `P_dis_max`, 放电使能, 通信故障 |
| 输出 | 储能放电功率需求（kW，正值） |
| 诊断 | `getLastOutput/getWindowAverage/isActive/hasAlarm` |

## 构建与运行

```bat
cd 03\需量管理控制器
scripts\build.bat       :: 编译生成 build\demand_sim.exe + build\demand_test.exe
scripts\run.bat         :: 编译 + 跑 demand_sim.csv + 出 figs\demand_sim_response.png
```

## 仿真场景

| 场景 | CSV | PNG |
|---|---|---|
| 综合工况（基线） | `data/demand_sim.csv` | `figs/demand_sim_response.png` |
| 已越限工况 | `data/demand_sim_over.csv` | `figs/demand_sim_over_response.png` |
| 长时间稳态 | `data/demand_sim_long.csv` | `figs/demand_sim_long_response.png` |
| 性能基准 | `data/demand_sim_perf.csv` | `figs/demand_sim_perf_response.png` |

详细设计见 [`docs/design.md`](./design.md)（原 *需量管理控制器设计文档.md*）。
