# 防逆流控制器

控制储能充电动作，避免关口功率反向输送电网（倒送）。当 `P_grid < P_grid_min` 时，
按 PI + 光伏前馈增大储能充电指令，使电网侧始终保持从电网吸收功率。

| | |
|---|---|
| 入口 | `src/AntiReverseController.{h,cpp}` |
| 算法 | PI + 光伏前馈 + 死区迟滞 + 遇限削弱积分 + 陈旧积分清零 |
| 控制周期 | 100 ms（`cfg.Ts`） |
| 输入 | `P_grid`（kW，正=电网供电）, `P_pv`, `P_chg_max`, `charge_enabled` |
| 输出 | 储能充电功率需求（kW，正值） |
| 状态 | 积分项、前一拍光伏、上一次输出、死区状态、首拍标志 |

## 构建与运行

```bat
cd 03\anti_reverse_controller
scripts\build.bat       :: 编译生成 build\ar_sim.exe + build\test_ar.exe
scripts\run.bat         :: 编译 + 跑 data\sim_sc1_pv_step.csv + 出 figs\sim_plot.png
```

## 仿真场景

| 场景文件 | 触发逻辑 |
|---|---|
| `data/sim_sc1_pv_step.csv` | 光伏阶跃扰动 |
| `data/sim_sc2_load_drop.csv` | 负荷突降 |
| `data/sim_sc3_pv_osc.csv` | 光伏小幅振荡 |
| `data/sim_sc4_no_reverse.csv` | 极小倒送风险（无逆流边界） |
| `data/sim_sc5_pv_drop.csv` | 光伏功率大幅跌落 |

详细设计见 [`docs/design.md`](./design.md)（原 *防逆流控制器设计文档.md*）。
