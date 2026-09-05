# 三控制器集成（integration）

> ⚠️ 原目录名 `三控制器集成`，因 MinGW g++ 工具链限制（`#include "..."` 中含中文字符会触发 `Invalid argument`），整合到英文目录 `integration`。

把防逆流、光伏出力平抑、需量管理三个实时控制器通过"防逆流绝对优先 + 需量/平抑滞环切换"的合并逻辑串起来跑联调仿真。

| | |
|---|---|
| 入口 | `src/IntegrationMain.cpp` |
| 合并策略 | 防逆流 > 需量/平抑（按需切换，含 3 拍滞环防抖） |
| 控制周期 | 100 ms（`Ts = 0.1`） |
| 仿真时长 | 3000 步（300 s） |
| 输出 | `data/integration_sim.csv` + `figs/integration.png` |

## 构建与运行

```bat
cd 03\integration
scripts\build.bat       :: 编译生成 build\integration_sim.exe
scripts\run.bat         :: 编译 + 跑仿真 + 出图
```

## CSV 输出字段

```
Time_s,P_load_kW,P_pv_kW,P_grid_kW,P_bat_kW,SOC_pct,Rev_kW,Dem_kW,Sm_kW,Mode
```

| 字段 | 含义 |
|---|---|
| `Time_s` | 仿真时间（秒） |
| `P_load_kW` | 仿真场景给定的负荷功率 |
| `P_pv_kW` | 仿真场景给定的光伏功率 |
| `P_grid_kW` | 关口实测功率 = P_load − P_pv − P_bat_prev |
| `P_bat_kW` | 储能实际充/放电功率（含爬坡限速） |
| `SOC_pct` | 电池 SOC |
| `Rev_kW` | 防逆流控制器原始输出 |
| `Dem_kW` | 需量管理控制器原始输出 |
| `Sm_kW` | 光伏出力平抑控制器原始输出 |
| `Mode` | 合并模式 0=SMOOTH 1=CHARGE 2=DISCHARGE |

## 仿真场景（固定脚本，无外部 CSV）

| 时段 | 场景描述 |
|---|---|
| 0 – 50 s | 负荷 200 kW，光伏 150 kW：稳定工况 |
| 50 – 100 s | 光伏阶跃到 300 kW：观察光伏消纳行为 |
| 100 – 200 s | 负荷阶跃到 450 kW：观察需量管理是否抑制窗口平均 |
| 200 – 300 s | 恢复初始负荷与光伏 |

## 涉及源码

集成项目**仅**包含 `IntegrationMain.cpp` 与合并逻辑。它通过 `-I` 引用三个控制器的源码：

```
../防逆流控制器/src/AntiReverseController.h
../光伏出力平抑控制器/src/SmoothingController.h
../需量管理控制器/src/DemandController.h
../shared/clamper.h
```

> 集成项目**不再复制控制器源码**，所有改动同步到原 controller 子项目。
