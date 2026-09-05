# 光伏出力平抑控制器

通过一阶低通滤波 + SOC 方向性衰减，平滑光伏出力波动对电网的冲击。

| | |
|---|---|
| 入口 | `src/SmoothingController.{h,cpp}` |
| 算法 | 一阶低通（`tau` + `Ts`）+ SOC 方向性衰减 + 充放电使能限幅 |
| 控制周期 | 100 ms（`cfg.Ts`） |
| 输入 | `P_pv`, `SOC`, `P_chg_max`, `P_dis_max`, 充/放电使能, 平抑使能, 数据有效 |
| 输出 | 补偿功率（kW，正=放电，负=充电） |

## 构建与运行

```bat
cd 03\光伏出力平抑控制器
scripts\build.bat       :: 编译生成 build\smoothing_sim.exe
scripts\run.bat         :: 编译 + 跑 S1~S6 六个场景 + 出 figs\S1~S6.png
```

## 仿真场景

| 场景 | CSV 文件 | PNG |
|---|---|---|
| S1 正常工况 | `data/smoothing_S1_normal.csv` | `figs/S1_normal.png` |
| S2 高 SOC（充电方向衰减） | `data/smoothing_S2_highSOC.csv` | `figs/S2_highSOC.png` |
| S3 低 SOC（放电方向衰减） | `data/smoothing_S3_lowSOC.csv` | `figs/S3_lowSOC.png` |
| S4 通信故障（数据无效退化） | `data/smoothing_S4_commfault.csv` | `figs/S4_commfault.png` |
| S5 平抑功能反复切换 | `data/smoothing_S5_toggle.csv` | `figs/S5_toggle.png` |
| S6 粗糙预测（敏感度测试） | `data/smoothing_S6_rough.csv` | `figs/S6_rough.png` |

详细设计见 [`docs/design.md`](./design.md)（原 *光伏出力平抑控制器设计文档.md*）。
