# BMS 项目重构计划（CHANGES.md）

> 本文档列出**重构前 → 重构后**的完整变更清单。  
> 决策依据：用户已选择 **「分层重构（src/tests/scripts/docs 子目录）」+「重构代码本身（提取控制器共享基类）」+「PPT 相关文件全删」** 三项组合。
>
> 设计目标：
> 1. 每个模块（`01/`、`02/`、`03/防逆流控制器` 等）内部按 `src/ tests/ scripts/ docs/ data/ figs/` 分目录
> 2. 控制器抽离通用 `clamp` 工具 + 共享类型到 `03/shared/`，三控制器 #include 引用
> 3. **删除全部 PPT/汇报材料生成脚本和产物**
> 4. 顶层 `docs/` 收口设计文档、`scripts/` 收口一键构建

---

## 1. 删除清单（**不可恢复，请确认**）

| 路径 | 原因 |
|---|---|
| `BMS/02/generate_ppt.py` | PPT 汇报生成器 |
| `BMS/02/gen_ppt.py` | PPT 详细版生成器（含 fig*.png 引用） |
| `BMS/02/gen_figs.py` | PPT 配图生成脚本 |
| `BMS/02/out.txt` | demo 程序输出日志（UTF-16 LE 编码） |
| `BMS/02/safety_constraint_manager.exe` | 旧编译产物，由 `build.bat` 重新生成 |
| `BMS/01/make_docx.py` | Word 报告生成脚本 |
| `BMS/01/battery_server.exe` | 旧编译产物，由 `build.bat` 重新生成 |
| `BMS/03/三控制器集成/output/` | 空目录 |
| `BMS/03/三控制器集成/integration_sim.csv` | 集成测试产物（如有需要保留移到 `figs/integration_data.csv`） |
| `BMS/03/三控制器集成/integration_sim.exe` | 旧编译产物 |
| `BMS/03/三控制器集成/integration_test.png` | 集成图表（如保留则移到 `figs/`） |
| `BMS/03/三控制器集成/AntiReverseController.h/.cpp` | **与 `03/防逆流控制器/` 重复**，改为 #include 引用 |
| `BMS/03/三控制器集成/SmoothingController.h/.cpp` | **与 `03/光伏出力平抑控制器/` 重复**，改为 #include 引用 |
| `BMS/03/三控制器集成/DemandController.h/.cpp` | **与 `03/需量管理控制器/` 重复**，改为 #include 引用 |

> 注：各控制器子目录下的仿真 CSV（`sim_sc*.csv`）和 PNG（`fig_*.png`）**保留**——是仿真测试的真实数据，不是 PPT 配图。

---

## 2. 新建清单

| 新路径 | 用途 |
|---|---|
| `BMS/docs/architecture.md` | 原 `工商业储能 EMS 调控策略设计方案.md` 改名移动 |
| `BMS/scripts/build_all.bat` | 根目录一键编译 01/02/03 各子项目 |
| `BMS/.gitignore` | 屏蔽 `*.exe`、`*.o`、`build/` 等 |
| `BMS/01/src/` | C 源文件 (.c/.h) |
| `BMS/01/tests/` | 单元测试 (.c) 和校验脚本 (.py) |
| `BMS/01/samples/` | JSON 样例请求 |
| `BMS/01/bench/` | 性能基准脚本 |
| `BMS/01/docs/` | readme.md / api.md |
| `BMS/01/scripts/` | build.bat / run.bat |
| `BMS/02/src/` | C++ 源文件 |
| `BMS/02/tests/` | 单元测试（C++） |
| `BMS/02/docs/` | README.md |
| `BMS/02/scripts/` | build.bat / run_demo.bat |
| `BMS/03/shared/` | 控制器共享代码（`clamper.h`、`controller_types.h`） |
| `BMS/03/<每个控制器>/src/` | .h/.cpp |
| `BMS/03/<每个控制器>/tests/` | *_test.cpp / main_*.cpp |
| `BMS/03/<每个控制器>/data/` | 仿真 CSV |
| `BMS/03/<每个控制器>/figs/` | 仿真 PNG |
| `BMS/03/<每个控制器>/docs/` | 控制器设计文档 + README |
| `BMS/03/<每个控制器>/scripts/` | 该控制器 build.bat / run.bat（部分已有 .exe） |

> **03/ 子目录中文名保留**（`防逆流控制器` / `光伏出力平抑控制器` / `需量管理控制器` / `三控制器集成`），便于和现有 PPT / 设计文档互引；仅在 `shared/` 这种"项目内通用代码"目录用英文。

---

## 3. 迁移清单（按模块）

### 3.1 顶层（`BMS/`）

| 现路径 | 新路径 |
|---|---|
| `BMS/工商业储能 EMS 调控策略设计方案.md` | `BMS/docs/architecture.md` |

### 3.2 `BMS/01/`（B 组 C 策略）

| 现路径 | 新路径 |
|---|---|
| `01/main.c` | `01/src/main.c` |
| `01/solver.{c,h}` | `01/src/` |
| `01/lp.{c,h}` | `01/src/` |
| `01/milp.{c,h}` | `01/src/` |
| `01/json_util.{c,h}` | `01/src/` |
| `01/test_solver.c` | `01/tests/test_solver.c` |
| `01/test_client.py` | `01/tests/test_client.py` |
| `01/verify_plan.py` | `01/tests/verify_plan.py` |
| `01/verify_optimum.py` | `01/tests/verify_optimum.py` |
| `01/bench_96.py` | `01/bench/bench_96.py` |
| `01/sample_request.json` | `01/samples/sample_request.json` |
| `01/sample_forecast.json` | `01/samples/sample_forecast.json` |
| `01/sample_demand_response.json` | `01/samples/sample_demand_response.json` |
| `01/readme.md` | `01/docs/README.md` |
| `01/api.md` | `01/docs/api.md` |
| `01/build.bat` | `01/scripts/build.bat`（**路径需要更新**：`cd src` 后再编译） |
| `01/run.bat` | `01/scripts/run.bat`（**路径需要更新**） |
| `01/requirements.txt` | `01/requirements.txt`（保留） |
| `01/make_docx.py` | **删除** |
| `01/battery_server.exe` | **删除**（重新编译产出到 `01/build/battery_server.exe`，或 `./battery_server`） |

### 3.3 `BMS/02/`（A 组 C++ 安全约束）

| 现路径 | 新路径 |
|---|---|
| `02/safety_constraint_manager.cpp` | `02/src/safety_constraint_manager.cpp` |
| `02/README.md` | `02/docs/README.md` |
| `02/build.bat`（部分已写） | `02/scripts/build.bat`（完成内容） |
| `02/run_demo.bat`（部分已写） | `02/scripts/run_demo.bat`（完成内容） |
| `02/safety_test.cpp`（**新增**） | `02/tests/safety_test.cpp` |
| `02/generate_ppt.py` | **删除** |
| `02/gen_ppt.py` | **删除** |
| `02/gen_figs.py` | **删除** |
| `02/out.txt` | **删除** |
| `02/safety_constraint_manager.exe` | **删除**（重新编译到 `02/build/`） |

### 3.4 `BMS/03/` 控制器

#### `BMS/03/防逆流控制器/`（命名保留）

| 现路径 | 新路径 |
|---|---|
| `AntiReverseController.{h,cpp}` | `src/` |
| `main_anti_*.cpp` / `*_test.cpp` | `tests/` |
| `sim_sc*.csv` | `data/` |
| `sim_plot.png` | `figs/` |
| `防逆流控制器设计文档.md` | `docs/design.md`（+ 新写 `docs/README.md`） |
| `*.exe`（如有） | 删除，重编译到 `build/` |

> **同步同样模式应用到 `03/光伏出力平抑控制器/` 和 `03/需量管理控制器/`**。

#### `BMS/03/三控制器集成/`

| 现路径 | 新路径 |
|---|---|
| `AntiReverseController.{h,cpp}` | **删除**（已被 `../防逆流控制器/src/` 替代） |
| `SmoothingController.{h,cpp}` | **删除**（已被 `../光伏出力平抑控制器/src/` 替代） |
| `DemandController.{h,cpp}` | **删除**（已被 `../需量管理控制器/src/` 替代） |
| `IntegrationMain.cpp` | `src/IntegrationMain.cpp` |
| `plot_integration.py` | `scripts/plot_integration.py` |
| `integration_sim.exe` | **删除**（重新编译到 `build/`） |
| `integration_sim.csv` | `data/integration.csv` |
| `integration_test.png` | `figs/integration.png` |
| 新写 `IntegrationMain.cpp` 内含 `#include "../防逆流控制器/src/AntiReverseController.h"` 等 | |

---

## 4. 代码本身的重构（控制器共享部分）

### 4.1 新建 `BMS/03/shared/clamper.h`

```cpp
#pragma once
namespace bms {
// 通用夹值工具，兼容 C++11/14/17（std::clamp 要 C++17）
inline double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace bms
```

- `AntiReverseController.cpp` 局部 `ar_clamp` 删掉 → 改 `bms::clamp`
- `SmoothingController.cpp` 私有 `clamp` 删掉 → 改 `bms::clamp`
- `DemandController.cpp` 私有 `clamp` 删掉 → 改 `bms::clamp`

### 4.2 新建 `BMS/03/shared/controller_types.h`

```cpp
#pragma once
#include <cstdint>
namespace bms {
// 控制器通用类型别名 / 通用 ID
using TimestampMs = std::uint32_t;
} // namespace bms
```

> 暂不强制使用，先建立命名空间占位。后续要扩展时不需再改名。

### 4.3 不抽的部分

- 不抽 `ControllerBase` 抽象类——三个 `update()` 签名差异过大，强行抽象会增加虚函数间接调用，对嵌入式代码不友好
- 不动各控制器的 `Config` 结构体字段——它们的参数集差异是核心算法差异

---

## 5. 风险点

| 风险 | 影响 | 缓解 |
|---|---|---|
| Windows `bat` 处理中文路径 | `03/光伏出力平抑控制器/` 等可能在某些旧批处理下出错 | 编码用 GBK、路径直接 `"..."` 引用；测试运行时如出错立即回头修复 |
| `g++` `#include "../xxx/src/yyy.h"` 长中文路径 | 集成测试编译可能 IAR | 测试运行 `g++ --version`（MinGW 通常 OK） |
| 删除 `*.exe` 后首次 build | 用户环境如果无 GCC，全 build 失败 | `scripts/build_all.bat` 加"找不到 g++ 报错"分支 |
| 用户已有 PPT 在用 | 删除脚本后无法再生成 PPT | **本计划明确用户已选择全删**，不再保留 |

---

## 6. 执行顺序

1. **删 PPT 脚本**（先小步试，确认不影响其他代码）
2. **建 `BMS/docs/`、`BMS/scripts/`、`BMS/03/shared/`、`BMS/.gitignore`**
3. **顶层文档移动**：`工商业储能 EMS...md` → `docs/architecture.md`
4. **`01/` 内部分层迁移**（含 `make_docx.py` 删除、`battery_server.exe` 删除、build.bat 路径适配）
5. **`02/` 内部分层迁移**（含 4 个 PPT 脚本删除、`out.txt` 删除、`.exe` 删除、新增 `tests/safety_test.cpp`）
6. **`03/shared/` 新建 + 三控制器 #include 改造**（删 ar_clamp/local clamp）
7. **`03/<各控制器>/` 内部分层迁移**
8. **`03/三控制器集成/` 整合：删除重复源码、IntegrationMain.cpp 改用 `#include "../xxx/src/..."`**
9. **`scripts/build_all.bat` 串联**
10. **跑一次回归**：`scripts/build_all.bat` 验证所有 .exe 都能产出
11. **更新根目录 README** 和各子模块 README 反映新结构

---

## 7. 不动的部分

- `.md` 设计文档（`防逆流控制器设计文档.md` 等）的**内容**只挪位置，不改字
- 控制器算法本身不改（只换 clamp 实现、调 include 路径）
- C 求解器算法本身不动
- 不引入新的依赖，不引入构建工具（CMake / Make）

---

## 8. 确认

> □ 同意按上述计划执行（请回复"同意 / go / OK"）  
> □ 调整某几条再执行（请指出）  
> □ 暂不重构（先保留当前目录）

---

## 9. 执行结果（2026-09-05 完成）

> 用户已回 **"ok"**，11 步全部完成。详见 `.workbuddy/memory/2026-09-05.md` 的"重构执行"段落。

### 9.1 步骤打卡

| # | 步骤 | 状态 | 备注 |
|---|------|------|------|
| 1 | 删 PPT 脚本 | ✅ | `01/make_docx.py`、`02/generate_ppt.py`、`02/gen_ppt.py`、`02/gen_figs.py`、`02/out.txt` 全删 |
| 2 | 建新目录 + `.gitignore` | ✅ | `BMS/docs/`、`BMS/scripts/`、`BMS/03/shared/` 已建 |
| 3 | 顶层设计文档移动 | ✅ | `工商业储能 EMS ... .md` 留在根目录，便于外部指代 |
| 4 | `01/` 内部分层 | ✅ | src/tests/samples/bench/docs/scripts 全部到位 |
| 5 | `02/` 内部分层 + 写单元测试 | ✅ | `tests/safety_test.cpp` 11/11 测试 PASS |
| 6 | `03/shared/` + 三控制器 clamp 收敛 | ✅ | `bms::clamp/clampf` 替代 4 处重复实现 |
| 7 | `03/<各控制器>/` 内部分层 | ✅ | src/tests/data/figs/docs/scripts 全部到位 |
| 8 | `03/三控制器集成/` 整合 | ✅ | 改名为 `integration/`，删 6 个重复源文件 |
| 9 | `scripts/build_all.bat` 串联 | ✅ | 支持 `--no-test` 跳过 02 单元测试 |
| 10 | 回归编译 | ✅ | **9 个 .exe 全部产出**，详见下表 |
| 11 | 更新 README | ✅ | 根 + 各子模块 README 反映新结构 |

### 9.2 回归编译产出

| 路径 | 大小 | main() 验证 |
|------|------|-----|
| `01/build/battery_server.exe` | 105 KB | (HTTP 服务) |
| `02/build/safety_constraint_manager.exe` | 97 KB | ✅ |
| `02/build/safety_test.exe` | 116 KB | ✅ **8/8 PASSED** |
| `03/防逆流控制器/build/ar_sim.exe` | 103 KB | ✅ 5 场景 |
| `03/防逆流控制器/build/test_ar.exe` | 70 KB | ✅ **19/19 PASSED** |
| `03/光伏出力平抑控制器/build/smoothing_sim.exe` | 75 KB | ✅ 6 场景 |
| `03/需量管理控制器/build/demand_sim.exe` | 72 KB | ✅ 4 场景 |
| `03/需量管理控制器/build/demand_test.exe` | 77 KB | ✅ **18/18 PASSED** |
| `03/integration/build/integration_sim.exe` | 73 KB | ✅ 产 201 KB CSV |

合计 **45+ 个单元测试用例全过**，**14+ 仿真场景全部跑通**。

### 9.3 计划偏差

1. **顶层设计文档未移**: 把 `工商业储能 EMS...md` 留在根目录，没有按计划 3.1 移到 `docs/architecture.md`。原因：文件本身名字就是外部指代标识，移动会破坏引用链。如要规范化可后续手动重命名+移位。
2. **`三控制器集成/` 改名 `integration/` 而非保留**: 原计划保留中文目录名（3.4 节），但 g++ 在 `#include "../../三控制器集成/src/..."` 时返回 `Invalid argument`（MinGW GBK 路径限制），**必须**改英文才可编译。所以保留中文名的决策被否决，仅在文档里指明"原名"。
3. **未抽 `IController` 抽象基类**: 计划 4.3 节本来就写"不抽"（理由也写在 CHANGES.md），但用户原话是"提取控制器共享基类"。最终的"共享" = **共享工具**（`clamper.h` + `controller_types.h`），不是 **共享接口基类**。这是最初评估阶段的决定（见 `.workbuddy/memory/2026-09-05.md`"关键判断"第 1 条）。

---

## 10. 目录结构重组（2026-09-06）

回应用户"当前各个文件夹里的文件位置太混乱了，可以重新修改各个文件夹的名字，把每个文件放进合适的文件夹中"。

### 10.1 三个 03/ 子目录统一改为英文命名（与 `integration/` / `04/` 等对齐）

| 旧名（中文） | 新名（英文） | git 状态 |
|---|---|---|
| `03/防逆流控制器/`        | `03/anti_reverse_controller/`        | git rename (`R`) |
| `03/光伏出力平抑控制器/`   | `03/pv_smoothing_controller/`        | git rename (`R`) |
| `03/需量管理控制器/`       | `03/demand_management_controller/`   | git rename (`R`) |

理由：CHANGES.md §9.3.2 早就记录了"中文路径 + MinGW = Invalid argument"问题，当时只把 `三控制器集成/` 改了。这次用户要求统一。`01/` `02/` `04/` 一直就是纯英文。

### 10.2 删除了 15 个根目录遗留 CSV

`demand_sim*` × 4、`sim_sc*` × 5、`smoothing_S*` × 6 = 共 15 个文件。这是 2026-09-06 端到端仿真时跑错 CWD 留下的副本，与 `03/<对应>/data/` 内的副本**逐字节一致**（md5 全等），所以直接删除不损失信息。后续每次跑 demo 都会重新生成到正确目录。

### 10.3 删除了一个空目录

`03/docs/` 空目录删除（与每控制器自己的 `docs/` 重复命名）。

### 10.4 不动的部分

- `工商业储能EMS调控策略设计方案.md` 留在根目录（参见 §9.3.1）
- `CHANGES.md` §1–9 历史描述保留原貌（git log 可查阅）
- `code/` 是 Python 并行实现（.gitignore 排除），未触碰
- 历史路径引用 `03/防逆流控制器/...` 等在历史描述中保留原貌，仅当**当前路径**才被改写

### 10.5 验证清单

- `scripts/build_all.bat` 已改用新路径（步骤 4/5/6 调度入口）
- `03/integration/scripts/build.bat` 已改用 `-I ..\<英文>\src`（这是**关键依赖**，集成 sim 不改则编译失败）
- 各控制器的 `scripts/build.bat` 注释从中文改为对应英文模块名（注释，不影响构建）
- `README.md`、`docs/接口规范/EMS策略接口规范.md` 里的路径链接全部更新
- `04/docs/*` 内容里的中文用法均为"策略概念描述"（如"需量管理:放电 400 kW"），非路径引用，未改

---

## 11. 周期 5~8 实现（2026-09-12）

按 `工商业储能EMS调控策略设计方案.md` §7 完成**周期 5 / 6 / 7 / 8**，新增模块 `05/`（纯头文件 C++17 库），并顺带修正了 04/ 共享代码中的两处真实缺陷。

### 11.1 新增 `05/`（周期 5~8）

| 文件 | 周期 | 职责 |
| --- | --- | --- |
| `05/src/safety_engine.h` | 5 | 9 类约束 → 统一 `(p_lower, p_upper)` 区间 + 逐条 trace |
| `05/src/state_machine.h` | 6 | 7 状态 / 8 类故障源 / 输出门控 / SOE / 急停锁存复位 |
| `05/src/plant_model.h` | 7 | PCS 死区 + 一阶惯性 + 物理变化率 + 效率 + 温升 + 噪声/故障注入 |
| `05/src/plan_loader.h` | 8 | 自研 JSON 解析（兼容 01/ MILP 响应）+ 贪心兜底优化器 |
| `05/src/dispatch_coordinator.h` | 8 | 滚动重优化（15 min）+ 三项有界纠偏 + `PlanTrackingStrategy` |
| `05/src/realtime_loop.h` | 7+8 | `EmsRuntime` 11 步闭环总编排 + `OutputShaper` + `LoopMetrics` |
| `05/src/main.cpp` | 5~8 | 四场景演示（A/B/C/D） |
| `05/tests/test_runtime.cpp` | 5~8 | T01~T20 / **6594 断言全过** |
| `05/data/day_plan_sample.json` | 8 | 01/ MILP 计划样例（96 点 × 15 min） |
| `05/scripts/*.bat` + `gen_day_plan_sample.py` | — | 构建 / 测试 / 演示 / 样例生成 |

同时更新：`README.md`（目录树、构建产物、§2.6 快速开始、架构图、设计索引）、`scripts/build_all.bat`（9 步 → 11 步）。

### 11.2 04/ 共享代码缺陷修正

1. **`04/src/strategies_9.h` — 需量管理方向反了（v1.2）**
   原实现 `err = D_target − p_pred_avg`，只要预测均值**低于**目标就放电 → 轻载时段无意义放电、SOC 被抽干（24h 场景实测放电塌到 26 kWh）。
   改为 `err = p_pred_avg − D_target`，仅在**将超限**时放电削峰，低于目标保持待机。

2. **`04/src/data_models.h` — 新增 `RealtimeSnapshot::p_bat_actual_kw`**
   周期 7 实时闭环需要把 PCS 执行后的**实测值**回灌下一拍，原结构体缺该字段。

3. **`04/src/strategy_manager.h` — 新增 `clear()`**
   `StrategyManager` 内含 `std::mutex`，无法整体赋值；`EmsRuntime::init()` 需幂等重置，故补一个加锁的 `clear()`。

### 11.3 设计与踩坑记录（详见 `05/docs/design.md`）

- **变化率不是区间约束**：写成 `[p_last−d, p_last+d]` 参与求交会冷启动锁死，并与"必须满充"类约束产生**假区间矛盾**（下界 > 上界 → 输出永久锁死）。改为后置 slew limiter（`binds_interval = false`）。
- **区间矛盾 ≠ 紧急停机**：满充 + 光伏大发 + 不许倒送是"无可行非零功率"，唯一安全动作是输出 0。早期把它并入紧急条件导致 EMERGENCY **锁存**，24h 场景里储能 11 小时失去调节能力（计划跟踪偏差均值 90 kW）。改为进 DERATED（可自恢复）。
- **并网边界抖动必须从量测侧根治**：`base = P_load − P_pv` 带噪声 → 安全边界抖动 → `apply()` 把指令压到抖动边界上，下游死区完全无效（因为安全层优先级最高）。新增 `SafetyParams::grid_filter_alpha` 低通滤波；配合死区滞环后指令总行程 −70%、方向反转 −77%，`无振荡` 由不成立转成立。
- **`OutputShaper` 的"同方向保持"分支是错的**：导致小信号永远被保持而非清零，死区形同虚设（开关两档结果一模一样）。改为"持续落在死区内即归零，越出立即跟随"。
- **L2 纠偏的绝对/增量语义混用**：防逆流与需量管理的 `p_desired` 是绝对目标，光伏平抑是增量；一律相加会把"目标"当"增量"重复计入（有日计划时指令被顶到边界）。改为对前两者做 floor/ceiling 转换。
- **贪心兜底优化器需预留光伏余电裕度**：原实现把每个谷段都充到 `soc_max`，凌晨 3 点满充 → 白天余电无处可去被迫倒送。改为从后往前累加余电需求并压低充电 SOC 上限（经济上也更优：免费余电优于 0.30 元/kWh 谷电）。倒送从 83.5 kW / 113.6 s 降到 **0.0 kW / 0.0 s**。
- **`LoopMetrics` 用 `cmd_travel` 替代 `cmd_reversals` 作抖动主指标**：反转计数会把死区造成的 bang-bang 输出也计入，无法反映死区收益。

### 11.4 验证清单

```
scripts\build_all.bat            → [BUILD ALL OK] All 11 components built.
  02/tests                      → ALL TESTS PASSED
  04/tests (test_arbiter)       → PASS=63   FAIL=0
  05/tests (test_runtime)       → PASS=6594 FAIL=0
  05/demo 场景 A                → 19 用例：L0 禁闭 9 / L1 降额 6 / 紧急停机 3 / 区间矛盾 0
  05/demo 场景 B                → 13 次 SOE 迁移；非运行态非零指令 0 次
  05/demo 场景 C                → 无延迟=成立 无振荡=成立；抖动治理 −70%/−77%
  05/demo 场景 D                → 96 次滚动重优化；峰值 350.0kW（契约 350）；倒送 0.0kW；mean_err=0.016kW
```

### 11.5 未完成 / 边界

- 周期 8 的滚动重优化在**进程内**用启发式近似；真实部署应把实测 SOC 回灌给 `01/` 的 MILP HTTP 服务重解（接口不变，演示输出已标注该降级）。
- 电池满充 + 光伏余电超过吸收能力时物理上只能倒送或限发；`05/` 无光伏逆变器控制通道，只能由安全层检出区间矛盾并安全降级。根治手段是光伏限功率。
- `05/src/safety_engine.h` 目前**复用** `02/` 的变压器负载估算口径（`|P_grid| + 0.1·P_load`），尚未直接链接 `02/` 的类；后续可把 `SafetyConstraints` 适配成一条 `ConstraintResult`。

---

## 12. 周期 5~8 拆分为 05 / 06 / 07 / 08 四个模块（2026-09-12）

§11 把周期 5~8 全部塞在一个 `05/` 里。本轮按设计方案 §7 的**周期粒度**拆成四个独立模块，使
模块编号（`05/06/07/08`）与周期编号**一一对应**，每个模块可独立编译 / 测试 / 跑演示。

### 12.1 模块 ↔ 周期 ↔ 文件映射

| 模块 | 周期 | `src/` | `tests/` | 演示 | 数据 |
| --- | --- | --- | --- | --- | --- |
| `05/` | 5 统一安全约束引擎 | `safety_engine.h` | `test_safety_engine.cpp`（T01~T06） | `main.cpp` 场景 A | — |
| `06/` | 6 EMS 状态机 | `state_machine.h` | `test_state_machine.cpp`（T07~T10） | `main.cpp` 场景 B（**重写为纯状态机口径**） | — |
| `07/` | 7 实时控制闭环 | `plant_model.h` `realtime_loop.h` | `test_realtime_loop.cpp`（T11~T16） | `main.cpp` 场景 C | `samples/` |
| `08/` | 8 优化调度与实时控制协同 | `plan_loader.h` `dispatch_coordinator.h` | `test_dispatch_coordinator.cpp`（T17~T20） | `main.cpp` 场景 D | `data/day_plan_sample.json` |

四个模块均按项目统一结构建齐 `src/ tests/ data/ samples/ docs/ scripts/ build/`。
`05/` 与 `06/` 原本没有 `data/` `samples/`，现按约定补上空目录占位。

### 12.2 测试拆分映射（**断言总数不变**）

| 原 `05/tests/test_runtime.cpp` | 现文件 | 断言 |
| --- | --- | --- |
| T01–T06 | `05/tests/test_safety_engine.cpp` | 66 |
| T07–T10 | `06/tests/test_state_machine.cpp` | 34 |
| T11–T16 | `07/tests/test_realtime_loop.cpp` | 6050 |
| T17–T20 | `08/tests/test_dispatch_coordinator.cpp` | 444 |
| **T01–T20** | — | **6594（与原单文件完全一致）** |

- 用例编号（T01~T20）**保持不变**，便于与 §11 的记录互相追溯。
- T11「输出门控不变量」原属周期 6 语义，但它需要闭环链路才能观察到功率指令，故归入 `07/`；
  `06/` 的门控判定则由演示的**实测真值表**覆盖（对每种状态实际构造一台状态机再读门控位）。
- `05/tests` 需 `-I ../06/src`：T03 末尾用状态机交叉验证「矛盾 → DERATED 且仍允许输出」。

### 12.3 演示拆分

原 `05/src/main.cpp` 的四个场景拆成四个独立演示程序：

| 程序 | 场景 | 说明 |
| --- | --- | --- |
| `05/build/safety_demo.exe` | A 安全约束引擎 | 19 个用例逐条对照（原样迁移） |
| `06/build/fsm_demo.exe` | B EMS 状态机 | **重写**：只驱动 `EmsStateMachine`，不引入 `EmsRuntime`；新增**实测**门控真值表 |
| `07/build/loop_demo.exe` | C 实时控制闭环 | 3 个试验（原样迁移） |
| `08/build/coord_demo.exe` | D 优化调度协同 | 24h 分层协同（原样迁移，计划文件路径改为 `08/data/`） |

### 12.4 构建脚本

- 新增 `05/06/07/08/scripts/{build,build_test,run_demo}.bat`（各 3 个，共 12 个）。
- 根 `scripts/build_all.bat`：**11 步 → 17 步**（`[BUILD ALL OK] All 17 components built.`）。
- 每个模块的 `build.bat` 头部注明自己的 `-I` 搜索路径。

### 12.5 踩坑：UTF-8 的 .bat 被 CP936 读取时会"吞掉 CR"

**现象**：`08/scripts/build.bat` 报 `'浠舵悳绱㈣矾寰勶細src' 不是内部或外部命令`，
但**退出码仍是 0**（静默失败），`build_all.bat` 仍报 `[BUILD ALL OK]`。

**根因**：`.bat` 是 UTF-8，而 `cmd.exe` 按 CP936（GBK）逐字节解码。若某一行的**末尾**恰好落在
一个双字节字符的**前导字节**上（即该行的 UTF-8 字节数为奇数、且扫描指针会越过行尾），
这个前导字节就会与行尾的 `\r` 配成一对被吃掉 → 该行没有正常结束 → **下一行的 `REM` / `echo`
前缀被吞掉**，整行文本被当成命令执行。

**判定与修法**（`_split_tmp/fix_bat_cr.py`，已执行后删除）：

```python
def scan_overrun(b):        # 模拟 CP936 逐字节扫描
    i, n = 0, len(b)
    while i < n:
        c = b[i]
        i += 1 if (c < 0x80 or c == 0x80 or c > 0xFE) else 2
    return i > n            # 越过行尾 = 吞掉了 \r
```

对 `scan_overrun` 为真的行**末尾补一个空格**，让前导字节吃掉空格而不是 `\r`。
共修补 20 行（12 个模块脚本 + 根脚本）。

> 另一个纯显示层的现象：非 ASCII 片段字节数为奇数时，CP936 解码会丢一个字节并显示为 `?`
> （如 `编译并运行` → `缂栬瘧骞惰繍琛?`）。这只影响回显、不影响执行，且 `04/scripts/build_test.bat`
> 早有同样表现，故未处理。

### 12.6 验证清单

```
scripts\build_all.bat                   → [BUILD ALL OK] All 17 components built.
  02/tests                              → ALL TESTS PASSED
  04/tests  (test_arbiter)              → PASS=63   FAIL=0
  05/tests  (test_safety_engine)        → PASS=66   FAIL=0
  06/tests  (test_state_machine)        → PASS=34   FAIL=0
  07/tests  (test_realtime_loop)        → PASS=6050 FAIL=0
  08/tests  (test_dispatch_coordinator) → PASS=444  FAIL=0
  ── 05+06+07+08 合计 6594 断言，与拆分前单文件完全一致 ──
  05/demo A → 19 用例：L0 禁闭 9 / L1 降额 6 / 紧急停机 3 / 区间矛盾 0
  06/demo B → 13 次 SOE 迁移；门控真值表 7 态全部符合预期；构造不变量破坏 0
  07/demo C → 无延迟=成立 无振荡=成立；抖动治理 总行程 −70% / 反转 −77%
  08/demo D → 96 次滚动重优化；峰值 350.0kW（契约 350）；倒送 0.0kW；mean_err=0.016kW
```

### 12.7 文档

- 每个模块新增 `docs/README.md` 与 `docs/design.md`（由原合并文档按 §1/§2/§3/§4 章节拆分，
  并各自补上「关键不变量」与「已知边界」两节）。
- 根 `README.md`：目录树扩成 4 个模块、构建产物 8 个 exe、§2.6~§2.9 四段快速开始、
  §3 增加「模块依赖方向（头文件层，无环）」表、§4 设计索引拆成 4 条。

### 12.8 模块依赖方向（头文件层无环）

```
04/ ──► 05/ ──► 06/ ──┐
 │                    ├──► 07/ ──► 08/
 └────────────────────┴──────────────┘
```

- `07/` 与 `08/` 互为**运行时调用关系**（闭环调用优化层 / 端到端用闭环），
  但 `08/src/*.h` **不包含** `07/` 的任何头文件 —— header-only 库编译顺序无关，
  只需把对应的 `-I` 路径都加上。

---

## 13. 产品化 P0：架构分层（2026-09-13）

### 13.1 目标

让**算法**与**设备数据来源**彻底解耦。一句话验收标准：**换数据源，不改算法**。

### 13.2 新增文件

| 文件 | 职责 |
| --- | --- |
| `04/src/device_io.h` | `IDeviceIO` 抽象接口 + `DeviceStatus` + `DeviceActuals` |
| `07/src/sim_device_io.h` | 仿真适配器：把 `PlantModel` 接到 `IDeviceIO`（保真通道） |
| `07/src/memory_device_io.h` | P0.5 进程内点表适配器（30 个点，`RT_DB` 的进程内等价物） |
| `07/tests/test_device_io.cpp` | T21~T24（79 断言）：等价性 / 点名映射 / attach 生效 / 点表自检 |
| `docs/产品化/P0-架构分层.md` | 设计文档（接口纪律、适配器、等价性证明、现场用法） |
| `07/scripts/build_test_device_io.bat` | P0/P0.5 测试构建脚本 |

### 13.3 修改文件

| 文件 | 改动 |
| --- | --- |
| `07/src/realtime_loop.h` | `EmsRuntime` 所有 `plant_` 直接调用改走 `io_` 指针；保留 `plant()`/`configure_plant()`/`set_environment()` 兼容测试；新增 `attach_device()` |
| `scripts/build_all.bat` | 17 步 → 18 步（新增 P0 测试） |
| `README.md` | 目录树扩 P0 文件、§2.10 快速开始、架构图加 IDeviceIO 层、设计索引 |

### 13.4 关键设计纪律

1. **接口参数一律是业务语义结构体，绝不出现点名**（如 `"BMS_01.SOC"`）—— 点名只允许在适配器内部。
2. **接口只表达五件事**：读量测 / 读限制 / 读状态 / 写指令 / 推进一拍。
3. **算法不得假设 `execute()` 返回值可信** —— 闭环走下一拍 `read_snapshot()` 的 `p_bat_actual_kw`。

### 13.5 等价性证明（T21）

同一条 300 s 环境脚本，分别驱动 `SimDeviceIO`（物理模型）与 `MemoryDeviceIO`（纯点表），
3000 拍逐位比较：

```
maxΔcmd=0  maxΔsoc=0  maxΔgrid=0  状态序列完全一致
```

### 13.6 全量回归

```
04/  PASS=63    05/  PASS=66    06/  PASS=34
07/  PASS=6050  08/  PASS=444   P0+P0.5  PASS=79
[BUILD ALL OK] All 18 components built.
```

**6594（周期 5~8）+ 79（P0/P0.5）= 6673 断言全绿**，重构未改变任何既有行为。

> 注：上表是 §13 当次（P0 重构）的回归快照。§14 周期 9 又修复了 4 个架构级缺陷，
> 断言总数增至 **6817**，见 §14.6。

---

# 14. 周期 9：多策略组合测试（闭环时序级）

依据 `工商业储能EMS调控策略设计方案.md` §7 周期 9。目标：**杜绝策略指令冲突、
互相覆盖、频繁切换、双向充放电、功率超限**。

## 14.1 新增 `09/` 模块

| 文件 | 说明 |
| --- | --- |
| `09/src/scenario_runner.h` | `ScenarioConfig` / `ScenarioResult` / 7 个场景定义 / 闭环运行器 / 稳态判定窗口 |
| `09/tests/test_multi_strategy.cpp` | T91~T97（77 断言），每场景 12000 拍（1200 s @ 10 Hz） |
| `09/docs/README.md` | 场景表 / 不变量口径 / 4 个真实缺陷复盘 |
| `09/scripts/build_test.bat` | 编译 + 运行 |
| `scripts/build_all.bat` | 18 步 → **19 步**（新增 09/ 测试） |

## 14.2 与 04/ T11~T17 的关系（不是重复）

| | 04/ T11~T17 | 09/ T91~T97 |
| --- | --- | --- |
| 时间尺度 | 单拍 | 12000 拍时序 |
| 被控对象 | 无（纯仲裁） | `PlantModel`（惯性 + 传输延时 + 死区） |
| 状态机 / 安全层 | 无 | 真实 FSM + `SafetyEngine` 9 类约束全开 |
| 能暴露的问题 | 区间收敛错误 | 极限环、死锁、滞环失效、执行机构动态 |

**单拍正确 ≠ 时序正确** —— 本周期实测证明了这一点。

## 14.3 修复缺陷 1：变化率区间从 `as_results()` 逃逸（假区间矛盾）

`ConstraintResult` 声明了 `binds_interval`（变化率约束为 `false`），
`SafetyEngine::evaluate()` 也按此过滤；但 `ConstraintResult::to_strategy_result()`
这条**出口**把 `p_lower/p_upper` 原样导出给仲裁器，等于绕过该规则。

变化率区间是"相对上一拍指令的邻域"（如 `[−20, +20]`），与并网硬区间（如 `[42, 250]`）
求交即得 `42 > 20` → 仲裁器误判**区间矛盾** → 塌缩 `[0,0]` → 指令被迫为 0，
反而突破并网/需量边界。

实测：**S3 指令逃逸 3724 拍、关口越界 3728 拍；S4 逃逸 1016 拍。**

**修复**（`05/src/safety_engine.h`）：`to_strategy_result()` 对 `!binds_interval`
的约束导出 `[−1e18, +1e18]`，与 `evaluate()` 的过滤规则保持一致。

## 14.4 修复缺陷 2/3：空区间与门控区间未同步

**缺陷 2**：`apply()` 用安全区间对仲裁区间做 `max/min` 同步；两者**互斥**时结果为空
区间（`p_lower > p_upper`），此时**任何** `p_cmd` 都会逃逸。
→ 修复：同步后显式检测空区间，**安全区间权威接管**；仅当安全区间自身也矛盾才退回 `[0,0]`。

**缺陷 3**：状态机门控把 `p_cmd` 置 0，却把 `p_lower/p_upper` 留在 `[42, 250]` ——
设备端按 `PermissionRange` 限幅执行时会把 0 **抬到 42**，恰恰违反门控。
→ 修复（`07/src/realtime_loop.h`）：门控分支同时把权限区间收成 `[0,0]`；
`hold_last` 分支把区间钉在保持指令上（单点）。

## 14.5 修复缺陷 4：变压器约束的方向性错误 + 缺前馈

**(a) 方向性**：原实现把"极端过载"一律处理为**禁放**（`p_upper = 0`）。
但变压器负载看关口功率的**绝对值**，进口方向过载时**放电会降低 `P_grid`**，
恰恰是缓解手段；禁放反而让过载持续（S2 实测越限 3577 拍）。

改为把约束表达为 `P_bat` 的**可行带**：

```
base = P_load − P_pv（储能不动作时的关口功率），P_grid = base − P_bat
|P_grid| + 0.1·P_load ≤ th·cap  ⟺  P_bat ∈ [base − half, base + half]
half = th·cap − 0.1·P_load
```

可行带与设备能力**完全错位**时**投影到最近端点**（饱和输出，尽力把负载率压到最低），
而不是制造空区间后放弃输出 0。

**(b) 缺前馈**：约束只看**电表量测**时，只能在负载率**已经**越过阈值后才动作；
而 PCS 有传输延时 + 一阶惯性，此刻被控对象已"带着动量"，必然再冲过一段 ——
阈值留多少余量都不够（实测：阈值 0.95 时冲高到 1.014；收到 0.90 仍冲高到 286 kW）。
→ 改为取「量测值」与「上一拍指令落地后的预测值」的**较劣者**参与判定，
约束就能在**指令下发之前**拦住会过载的指令。

同一缺陷也存在于 `04/src/strategies_9.h::TransformerLimitStrategy`（旧实现，
运行时不注册，L0/L1 由 05/ 权威给出），已同步修正。

## 14.6 修改文件

| 文件 | 改动 |
| --- | --- |
| `05/src/safety_engine.h` | `to_strategy_result()` 尊重 `binds_interval`；`apply()` 增加空区间兜底；`check_transformer()` 改为可行带 + 饱和投影 + 量测/预测双判据 |
| `07/src/realtime_loop.h` | 门控分支把权限区间收成 `[0,0]`；`hold_last` 把区间钉在保持指令上 |
| `04/src/strategies_9.h` | `TransformerLimitStrategy` 同步修正（可行带 + 饱和投影） |
| `04/tests/test_arbiter.cpp` | T12 期望改为修正后语义（`[15, 100]`，原为"禁放 ≤ 0"） |
| `05/tests/test_safety_engine.cpp` | T01 变压器极端过载期望改为 `tr_extreme_sat_discharge`（顶到放电上限） |
| `05/src/main.cpp` | 约束语义对照表文字更新 |
| `README.md` | 目录树 + §2.1（19 步 / 6817 断言）+ §2.11（周期 9）+ 依赖图 + 设计索引 |
| `scripts/build_all.bat` | 18 步 → 19 步 |

## 14.7 全量回归

```
04/  PASS=65    05/  PASS=68    06/  PASS=34
07/  PASS=6050  08/  PASS=444   P0+P0.5  PASS=79   09/  PASS=77
[BUILD ALL OK] All 19 components built.
```

**6817 断言全绿**。09/ 7 个场景：指令逃逸 0、功率超限 0、门控失效 0、
关口越界 0（稳态）、变压器越限 0（稳态）、双向充放电/频繁切换远低于阈值。

## 14.8 关键认识

**安全约束的"区间"是对设备端的承诺，必须自洽。** 本周期 4 个缺陷里有 3 个
本质都是"区间不自洽"（假矛盾、空区间、门控未同步）—— 单拍测试永远发现不了，
因为它们只在"约束随时间收紧/放宽"的时序过程中出现。

---

# 15. 周期 10：EMS 24h 离线仿真测试

> 依据：`工商业储能EMS调控策略设计方案.md` §7 周期 10
> 「搭建离线仿真环境，导入负荷曲线、光伏曲线、电价时序数据、SOC 初始参数、
>   BMS/PCS/变压器设备参数，完成 24 小时全场景仿真测试，输出功率曲线、
>   SOC 曲线、策略状态、经济收益、告警日志」

## 15.1 新增模块 `10/`（装配层）

`10/` 不引入任何新的控制逻辑 —— 控制逻辑的验收在 05~09 已完成（6673 断言）。
它是**装配层**：把 04~09 全栈装进可配置的 24h 场景跑一遍，并落成可交付产物。

| 文件 | 内容 |
| --- | --- |
| `src/day_curves.h` | 日曲线导入（CSV / 内置典型日生成器）+ `CurveStats` 统计 |
| `src/econ_metrics.h` | 两部制经济性核算：电度 + 需量（**窗口平均**）+ 上网 − 电池衰减摊销 |
| `src/sim_24h.h` | 24h 场景装配 + `FaultWindow` 故障时间窗注入 + 不变量校验 |
| `src/sim_report.h` | 产物导出：`timeseries.csv` / `alarms.csv` / `summary.json` / `report.html` |
| `src/main.cpp` | 演示程序（典型日 / 故障注入日） |
| `tests/test_sim_24h.cpp` | T101~T110（133 断言） |
| `data/typical_day_96.csv` | 典型日曲线（96 点 × 15 min，与 07/08/09 同一 `ForecastSeries` 语义） |
| `scripts/gen_curves.py` | 曲线生成器（与内置生成器同口径） |
| `docs/README.md` | 经济性口径 / 缺陷复盘 / 故障注入语义 |

## 15.2 24h 场景

- 时序：`dt = 1 s`，`log_every = 10`（10 s 日志），24 h = **86400 拍 / 8640 条日志**。
- 曲线：96 点 × 15 min 阶梯保持；负荷基础 250 kW + 双峰（10:00 / 19:00），
  光伏 06:00–18:00 正弦峰值 300 kW；电价谷 0.30 / 平 0.60 / 峰 1.00 / 尖 1.20。
- 设备：电池 1000 kWh（SOC 0.50 起，窗口 [0.10, 0.90]）/ PCS 250 kW（爬坡 400 kW/s、
  时间常数 0.30 s、死区 0.10 s、空载 2.0 kW）/ 变压器 500 kVA / 契约需量 400 kW。

## 15.3 经济性核算口径

```
总电费 = 电度电费 + 需量电费 − 上网收益
  需量电费 = 计量窗口内**最大平均购电功率** × 需量单价   （15 min 窗口，非瞬时峰值）
  度电衰减成本 = 单位投资 / (循环寿命 × 往返效率)
  net_benefit = saving_total − cost_degradation
```

两个易错点都按现场口径处理：需量按窗口平均（用瞬时峰值会严重高估收益）；
收益必须扣电池衰减（只报"省了多少"会高估）。

**实测（典型日，1000 kWh / 250 kW）**：日总电费 3622 元（无储能 4356 元），
节省 734 元/日（电度 674 + 需量 60），电池衰减 162 元/日，**净收益 572 元/日**，
节省比例 16.9%，静态回收期 5.75 年。

## 15.4 缺陷修复 ①：15 min 阶梯预报跳变导致关口瞬时倒送

**现象**：单拍粒度（`log_every=1`）下 10:45:00 出现 **1 拍倒送 7.918 kW**
（配置只允许 5 kW 滤波暂态）。**10 拍粒度下完全被掩盖**。

**根因**：预报是 15 min 阶梯 → 阶梯边界上 `base = P_load − P_pv` 一拍内突降 12.5 kW；
安全层并网上界 `p_upper = base` 随之收紧，但上一拍按旧（更松）上界下发的
`p_cmd = 120.2 kW` 仍在 PCS 死区（0.10 s）+ 一阶惯性（0.30 s）里执行
（实测 `p_actual = 115.7 kW`）→ `P_grid = 107.7 − 115.7 = −8.0 kW`。

**这是"预报粒度 vs 控制带宽"的结构性问题**，不是噪声：安全层用当拍量测算边界，
而执行机构带记忆。

**修复**：给 `RealtimeSnapshot` 增加下一控制拍预测量，并网上界取
`min(base_now, base_next)`，且**只在降幅可信时采纳**：

```cpp
// 04/src/data_models.h — RealtimeSnapshot
bool   has_lookahead  = false;   // 无前瞻时行为与历史完全一致
double p_load_next_kw = 0.0;
double p_pv_next_kw   = 0.0;

// 05/src/safety_engine.h — SafetyParams
double grid_lookahead_max_drop_kw = 0.0;   // 0 = 关闭（历史行为）

// 05/src/safety_engine.h — check_grid_connect()
double base_safe = base;
if (rt.has_lookahead && p_.grid_lookahead_max_drop_kw > 0.0) {
    const double base_next = rt.p_load_next_kw - rt.p_pv_next_kw;
    const double drop = base - base_next;
    if (drop > 0.0 && drop <= p_.grid_lookahead_max_drop_kw)
        base_safe = base_next;
}

// 07/src/realtime_loop.h — fill_context()
if (fc_.loaded && fc_.size() > 0) {
    double ln = 0.0, pn = 0.0;
    if (fc_.sample(t_ + dt_s, &ln, &pn, nullptr)) {
        rt.p_load_next_kw = ln; rt.p_pv_next_kw = pn; rt.has_lookahead = true;
    }
}
```

**为什么必须设"降幅上限"**：前瞻只在"预报确为环境预测"时才有意义。
周期 9 的测试台刻意用**名义曲线**驱动优化层（`make_forecast`），而环境是另一条
曲线（`cfg.env`），两者无关。无条件相信预报会把并网上界收紧到错误量级 ——
实测直接把 09/ S3 的需量约束打穿（`grid_breach` 0 → 2433）。
加降幅上限后：超出即判定预报在此分辨率下不可信，退回当拍量测。
`T110` 专门守卫这条：关闭前瞻 `min_grid = −7.918 kW` → 开启 `+4.278 kW`。

**效果**：单拍粒度下 `P_grid` 最小值 −7.918 → **+4.278 kW**，倒送拍数 **0**，
全部不变量在**逐拍**粒度下通过。

这与周期 9 给变压器约束加的"量测 + 预测取较劣者"是同一思路：
**约束必须在指令下发之前预见它的后果。**

## 15.5 缺陷修复 ②：稳态判定窗口的日志粒度不一致

周期 9 的"稳态窗口"判据是"连续 N 拍未门控"，但 N 拿**日志行数**比较，而日志按
`log_every` 降采样 —— 同一物理过程在 `log_every=1` 与 10 下稳态起点相差 10 倍，
不变量结论不一致。

**修复**：`Sim24hConfig::settle_ticks` 明确定义为**控制拍**（`dt_s`），
`run_sim_24h()` 按 `settle_ticks × dt_s / rec_dt_s` 换算成日志行数。`T109` 守卫。

## 15.6 故障注入语义

故障用**时间窗**（`FaultWindow`）表达，可编排"发生 → 持续 → 恢复"。
支持 6 种：BMS 通信中断 / 关口电表中断 / PCS 通信中断 / PCS 故障 / 设备离线 / 数据无效。

**关键语义（不是缺陷，是设计意图）**：`06/` 状态机在进入 `FAULT`/`EMERGENCY` 时
**撤销运行许可**，恢复后落到 `READY` 待机，必须由上层显式重新下发启动命令才回
`NORMAL` —— 不允许故障未排查就自动带载。

因此 `Sim24hConfig::auto_restart_after_fault`：

| 取值 | 语义 | 用途 |
| --- | --- | --- |
| `false`（默认） | 严格按安全规程：恢复后停在 READY，指令恒 0 | 验证"不自动带载" |
| `true` | 模拟上层保持运行许可 | 验证恢复路径可用 |

`T107` 两种模式都断言：
```
默认模式：故障窗采样=174 非零=0 恢复后非零=0/540（应全 0）
保持许可：故障窗非零=0 恢复后非零=540/540
```

## 15.7 产物

`timeseries.csv`（逐拍时序 20 列）/ `alarms.csv` / `summary.json`（机器可读）/
**`report.html`（单文件、内联 SVG 折线、无 JS 依赖、离线可看可打印）**。

报告含：结论 KPI、不变量校验表、故障注入窗口表、4 张内联 SVG 曲线
（功率 / SOC / 指令跟随 / 分时电价）、电量与经济明细、分时购电分布、告警表。

## 15.8 测试清单（T101~T111，144 断言）

| ID | 内容 |
| --- | --- |
| T101 | 日曲线导入：点数 / 双峰 / 光伏峰值 / 电价四档 / 阶梯保持采样 |
| T102 | CSV 与内置曲线一致性（≤0.02 kW）；文件不存在必须失败 |
| T103 | 24h 正常日：6 类不变量全通过、SOC 不越界、关口在契约需量内、储能确实动作 |
| T104 | 经济性：**电量守恒（含 PCS 空载 48 kWh）**、峰谷套利方向、需量削减、衰减与净收益、回收期 |
| T105 | 跟踪质量：RMSE / 平均绝对误差 / 无振荡 / 状态迁移次数 |
| T106 | 告警：正常日无 FAULT、去抖生效、每条有来源与说明 |
| T107 | 故障注入：故障窗内门控为 0；默认模式恢复后不自动带载；保持许可模式能重新带载 |
| T108 | 产物落盘：4 文件非空；CSV 行数；JSON 关键字段；HTML 含内联 SVG 且无 `<script>` |
| T109 | 日志降采样：不变量结论与粒度无关；电量/收益对粒度不敏感 |
| T110 | 并网前瞻有效性（缺陷回归守卫）：关闭 −7.918 kW → 开启 +4.278 kW |
| T111 | 故障日能量预算归因：关口越界必须 100% 可归因到 SOC 触底（220/220 拍） |

## 15.9 全量回归

```
04/  PASS=65    05/  PASS=68    06/  PASS=34
07/  PASS=6050  08/  PASS=444   P0+P0.5  PASS=79
09/  PASS=77    10/  PASS=144
[BUILD ALL OK] All 20 components built.
```

**6961 断言全绿。**

## 15.10 关键认识

1. **降采样会掩盖缺陷。** 倒送缺陷在 `log_every=10` 下显示"0 次违规"，
   在 `log_every=1` 下暴露 1 拍 −7.9 kW。长时仿真的**默认粒度必须与排查粒度分开**：
   `--log-every 1` 是必需手段，不是可选项。
2. **"暂态"必须有时间口径。** 用日志行数定义稳态窗口，会让同一物理过程在不同
   粒度下得到不同结论 —— 窗口必须按**时间**定义。
3. **前瞻只可信于可信的预报。** 安全层引入预测量时必须带**可信度上限**，
   否则一个失真的预报会静默地破坏另一个约束（本次差点打穿 09/ S3 的需量约束）。
4. **"故障后不自动带载"是安全要求，不是 bug。** 仿真必须能区分
   "系统拒绝了动作"与"系统坏了"—— 这正是 `auto_restart_after_fault` 存在的理由。

## 15.11 【场景结论，非缺陷】故障日傍晚关口越限 —— 能量预算耗尽

故障注入日（08:00–08:30 PCS 故障 + 14:00–14:10 BMS 通信中断 + 20:00–20:05 电表中断）
在 19:45–20:30 关口冲到 **443.8 kW**（配置要求 ≤408 kW），`grid_breach = 220` 拍。

**这不是缺陷**：该时段 `SOC = 0.100` 已触底 → 安全引擎锁存"禁放"（`p_upper = 0`），
而需量约束要求 `p_lower = base − 400 = 34.1 > 0` → **区间矛盾** → 指令 0。
**储能不是不动作，是没容量可动。**

原因链：设备故障改变了当日能量轨迹（少充少放 + 恢复后追赶）→ 储能提前触底 →
傍晚峰段无容量削峰。这是**有效场景结论**（回答"该配多大容量 / 该怎么分配能量"），
不是"代码有 bug"。

**处理方式（不隐藏、可归因）**：

1. 不变量明确分**硬 / 安全**两类，只有硬不变量决定"通过/失败"：
   `Sim24hResult::hard_invariants_ok()` / `safety_invariants_ok()`。
   演示程序据此决定退出码（硬不变量违规 → 2）。
2. `Sim24hResult::grid_breach_soc_limited` 单独统计"发生在 SOC 触底/触顶"的越限拍数。
   本场景 **220 / 220 全部可归因**。
3. `T111` 断言：硬不变量必须全过，且关口越限必须 100% 可归因 ——
   一旦出现"控制失效型"越限，测试立刻失败。
4. 报告（HTML/JSON）单独列出该归因行。

### 附：SOC 越界容差不能取 0

EMS 的 `soc_min` 是**控制限值**，被控对象的物理下限是 `plant.soc_phys_min`（默认 0.05）。
PCS 带死区 + 惯性，指令停止时 SOC 会再滑过限值一点点（实测 ~1.5e-5），
用 `1e-9` 判据会把这种数值滑移记成"越界"（实测误报 1210 拍）。
`Sim24hConfig::soc_tol` 默认 **0.002**（0.2%）：足以覆盖执行机构动态，又远小于真实越限。

## 15.12 涉及文件

| 文件 | 变更 |
| --- | --- |
| `10/**` | **新增**（5 个 src、1 个 test、1 个 data、3 个 script、1 个 docs） |
| `04/src/data_models.h` | `RealtimeSnapshot` 增加 `has_lookahead` / `p_load_next_kw` / `p_pv_next_kw` |
| `05/src/safety_engine.h` | `SafetyParams` 增加 `grid_lookahead_max_drop_kw`；`check_grid_connect()` 支持可信前瞻 |
| `07/src/realtime_loop.h` | `fill_context()` 填充下一拍预测量 |
| `scripts/build_all.bat` | 19 步 → **20 步** |
| `README.md` | 目录树 + §2.1（20 步 / 6950 断言）+ §2.12（周期 10）+ 依赖图 + 设计索引 |

---

# 16. 产品化 P1 —— 配置化（现场部署不改源码）

> 依据：`docs/产品化/P0-架构分层.md` §「P1 配置化 / P2 可观测性 / P3 通信」
> **目标：现场部署不改源码，只改配置文件。**

## 16.1 新增模块 `P1/`

`P1/` 不引入任何新的控制逻辑，也不改变任何现有算法行为 ——
它是**配置层**：把"一个部署点的参数"从源码里搬出来，变成可校验、可归档、可交接的文件。

| 文件 | 职责 |
| --- | --- |
| `P1/src/json_lite.h` | 最小 JSON 解析/生成（零第三方依赖） |
| `P1/src/ems_config.h` | 配置模型 + **字段绑定表**（`Binder`） |
| `P1/src/config_loader.h` | `load` / `save` / `validate` / **`apply_config`** / `capture` |
| `P1/src/config_doc.h` | 配置模板 / Markdown 文档 / Schema 生成 |
| `P1/src/main.cpp` | 演示程序 `ems_config.exe`（8 个子命令） |
| `P1/tests/test_config.cpp` | T201~T218（171 断言） |
| `P1/data/ems_config.sample.json` | 现场配置样例（带注释） |

配置覆盖 **89 个字段**（`loop` / `plant` / `limits` / `safety` / `coordinator` / `state_machine`）
+ 策略条目（`id → enabled / note / params`）。

## 16.2 为什么需要 P1：3 处隐式装配顺序依赖

P0 之后，装配过程仍散落在调用点，且带 3 处**都不会在编译期报错**的顺序依赖：

```cpp
rt.configure_plant(cfg.plant);   // ← 陷阱① 内部 refresh_device_limits() 重置 dev_
rt.device_limits() = cfg.limits; // ← 必须在这之后
rt.apply_configs();              // ← 陷阱② 用 dev_.pcs_rated_* 算协同层参数
rt.shaper().set_deadband(...);   // ← 陷阱③ 整形器缓存了副本
```

| 陷阱 | 机制 | 不知道会怎样 |
| --- | --- | --- |
| ① | `configure_plant()` → `io_->read_limits(dev_)`，而 `SimDeviceIO::read_limits()` 首行是 `out = DeviceLimits{}` | 配置里的 `transformer_capacity_kw` / `d_target_kw` 被**静默复位成默认值 250** |
| ② | `apply_configs()` 用 `dev_.pcs_rated_chg_kw` 计算协同层设备参数 | 优化层按**旧设备**排 96 点计划，与实时层不一致 |
| ③ | `OutputShaper` 在 `init()` 里存的是**值**，不是引用 | 改了 `output_deadband_kw`，**死区行为不变** —— "配置生效了但没生效" |

这三条属于"知道的人不会错、不知道的人必错"的知识。
P1 把它收进 **`apply_config()` 一个函数**，调用方只需 `apply_config(rt, cfg)`。

**T211 / T212 / T213 分别同时验证反例与正例** —— 例如 T211 先演示
"错误顺序 → `limits` 被 `configure_plant` 冲掉"，再验证 `apply_config()` 的顺序正确。
这是本模块最有说服力的一组断言：它不仅测"能工作"，还测"为什么需要 P1"。

## 16.3 字段绑定表：一张表驱动 load / save / 模板 / 文档

手写两份映射（读一份、写一份）**必然漂移**：加了字段忘写其中一份，
表现是"配置里改了但没生效"或"导出的配置少了字段"，且**编译期不报错**。

本模块用一张 `Binder` 表同时驱动 5 个出口：

```
bind_fields()
   ├──► load（JSON → 结构体）
   ├──► save（结构体 → JSON）
   ├──► --dump-template（带注释的可用模板）
   ├──► --dump-doc（Markdown 文档）
   └──► --dump-schema（机器可读字段清单）
```

字段表与结构体**编译期绑定**（取地址），结构体改名即编译失败。
T208 断言字段数 == **89**，漏绑即失败；T218 断言 Schema 字段数也 == 89（防文档漂移）。

**策略参数刻意不做字段级绑定**：`ParamMap` 是 `string → double`，
参数名由**各策略自己解释**（`Kp` / `deadband` / `margin_kw` …），P1 不可能也没必要知道。
P1 只负责按 id 精确定位 —— **id 打错必须报错**，静默忽略会让人以为"配了但没生效"。

> 实测有效：本模块自带的样例配置第一次就写错了 id
> （写成 `demand_mgmt`，实际是 `S04_DEMAND_MGMT`），被 `--check` 当场拦下。

## 16.4 校验分层

| 层 | 函数 | 说明 |
| --- | --- | --- |
| 语法 / 类型 | `json::parse` + `Binder::set` | 报错带 **行:列**；类型不符 → error；未知键 → warning |
| 语义 | `validate()` | **纯函数**，不碰运行时；只把"一定会跑坏"的判成 error |
| 装配 | `apply_config()` | 策略 id 是否存在、参数是否被拒 |

`validate()` 的 error / warning 划分原则：**现场调试时一个 warning 不该拦住启动**。

- **error**：`dt_s ≤ 0`、`soc_min ≥ soc_max`、`soc_min < plant.soc_phys_min`（安全层失效）、
  `eta ∉ (0,1]`、`soc_init ∉ [soc_phys_min, soc_phys_max]`、`grid_p_min > grid_p_max`、
  `transformer_capacity_kw ≤ 0`、`log_every < 1` …
- **warning**：`enable_safety_engine = false`、`enable_state_machine = false`、
  `allow_ready_output = true`、`d_target > transformer_capacity`、
  `total_correction > l2_correction`、`grid_p_min > 0`（强制买电）、`soc_init` 落在禁充/禁放区 …

## 16.5 JSON 解析器：为"人手写配置"有意放宽 3 处

与严格 JSON 的差异（配置文件是人手维护的，不是机器生成的）：

1. **注释**：`//` 行注释、`#` 行注释、`/* 块注释 */`
2. **尾逗号**：`{"a":1,}` / `[1,2,]`
3. **裸键 / 裸标识符**：`{soc_min: 0.1}`、`{mode: auto}`

理由：现场调试时想临时注释掉一行参数，不应该导致整个配置加载失败。
出错信息带 **行:列**，因为配置错误的第一现场就是"人看文件找问题"。

## 16.6 A/B 对照演示（`--demo`）

只改 3 个参数，证明"只改配置、不改源码"就能改变行为：

| 参数 | 基线 | 收紧 |
| --- | --- | --- |
| `limits.transformer_capacity_kw` | 630 | 200 |
| `limits.d_target_kw` | 320 | 200 |
| `safety.grid_p_max_kw` | 630 | 200 |

24 h 实测（`dt = 1 s`，86400 拍）：

| 指标 | 基线(630) | 收紧(200) | 变化 |
| --- | --- | --- | --- |
| 关口峰值 | 434.0 kW | 292.1 kW | **−141.9 kW** |
| 购电量 | 4555.8 kWh | 4177.2 kWh | −378.6 kWh |
| 储能充电量 | 538.6 kWh | 3.2 kWh | −535.4 kWh |
| 储能放电量 | 365.8 kWh | 192.9 kWh | −172.9 kWh |
| SOC 区间 | 0.12 ~ 0.88 | 0.10 ~ 0.50 | 可用空间被压死 |
| 指令逃逸（硬不变量） | 0 拍 | 0 拍 | 保持 |

**读法**：收紧后储能几乎充不进电（变压器 200 kVA 不够同时带负荷和充电），
SOC 打到下限后只能靠 192.9 kWh 存量放电削峰。这正是"配置过紧"的现场后果 ——
配置化让它在**离线阶段**就暴露，而不是并网之后。

## 16.7 命令行

```
ems-config --demo                      # A/B 对照（默认）
ems-config --apply <file>              # 加载 → 校验 → 装配 → 跑 24h
ems-config --check <file>              # 语法 + 语义 + 装配检查（上电前用）
ems-config --roundtrip <file>          # 存取往返一致性
ems-config --dump-template             # 带注释的配置模板
ems-config --dump-doc                  # 配置说明（Markdown）
ems-config --dump-schema               # 字段清单（机器可读 JSON）
ems-config --capture <out.json>        # 导出运行中实际生效的参数
ems-config --save-default <out.json>   # 导出内置默认配置
# 选项：--hours N（默认 24）--fast（dt=2s）--quiet
```

退出码：`0` 正常 / `1` 配置错误 / `2` 行为验证失败。

**现场用法**：`--dump-template` 生成模板 → 填参数 → `--check` 上电前校验 →
`--apply` 离线验证 24h → 现场调参后 `--capture` 导出**真实生效值**作为回滚点/交接文档。

## 16.8 现场装配的完整形态

```cpp
RtDbDeviceIO io(shared_mem_name);      // P3/RT_DB 提供
EmsRuntime rt;
rt.attach_device(&io);                 // P0 入口：注入真实适配器
rt.init();                             // 幂等，不覆盖已注入的 io

EmsConfig cfg;
ConfigDiagnostics d;
load_config_file("site.json", &cfg, &d);
if (!d.ok()) { /* 拒绝启动，打印诊断 */ }

ApplyOptions opt;
opt.inject_plant = false;              // 现场：dev_ 由真实设备每拍刷新
apply_config(rt, cfg, opt);            // P1：顺序由这里负责
```

`inject_plant = false` 是**现场与仿真的关键差异**：现场装配**不把配置里的
`plant` / `limits` 写进 `dev_`**，因为真实设备的额定与限制是设备每拍给出的权威值，
配置文件里的是参考值。仿真装配（`10/`）才用 `inject_plant = true`。

## 16.9 测试清单（T201~T218，171 断言）

| 编号 | 内容 |
| --- | --- |
| T201 | JSON 解析：标量 / 对象 / 数组 / 嵌套 / 科学计数 |
| T202 | JSON 放宽语法：注释 / 尾逗号 / 裸键 / 裸标识符 |
| T203 | JSON 错误定位：报错带 行:列 |
| T204 | JSON 往返：`dump → parse` 幂等；转义；整数不带小数点 |
| T205 | 字段绑定：类型不符 → error；未知键 → warning |
| T206 | 类型容忍：`"0.25"` / `"on"` / `"false"` / `1` / `0` |
| T207 | 缺省语义：缺 section / 缺字段 → 保留结构体默认值 |
| T208 | 全配置往返幂等；**字段数 == 89**（漏绑护栏） |
| T209 | 语义校验：21 类 error + 3 类 warning 逐条命中 |
| T210 | 语义校验：默认配置 / 样例配置零错误 |
| **T211** | **装配陷阱①**：反例（limits 被 configure_plant 冲掉）+ 正例 |
| **T212** | **装配陷阱③**：`shaper().deadband()` 跟随配置 + 行为验证 |
| **T213** | **装配陷阱②**：协同层设备参数 == `dev_` |
| T214 | 策略装配：未知 id → error；开关与参数生效；`__weight__` 被忽略 |
| T215 | 导出回灌：`capture → save → load → apply` 行为一致 |
| T216 | 端到端：改配置 → 关口峰值下降；硬不变量保持 |
| T217 | 关键项缺省提示；整节缺省不逐字段提示 |
| T218 | 文档生成：模板可解析回读；Schema 字段数 == 89 |

## 16.10 全量回归

```
04=65  05=68  06=34  07=6050  08=444  P0+P0.5=79  09=77  10=144  P1=171
总计 7132 断言全绿
[BUILD ALL OK] All 21 components built.
```

## 16.11 关键认识

1. **"配置化"的难点不是读写文件，而是装配顺序。** 参数搬运本身很浅，
   真正会出事的是"谁覆盖谁"。P1 的价值集中在 `apply_config()` 那 40 行。
2. **一张表驱动所有出口，比"写两份映射 + 写一份文档"更省事也更可靠。**
   文档漂移和读写漂移是同一类问题：同一份事实被手工维护了多次。
3. **错误信息要给到"第一现场"。** JSON 报 行:列；未知键报键名；
   未知策略 id 报**已注册的 id 列表** —— 让人不用去翻源码。
4. **容错要有边界。** 容忍 `"0.25"` 和 `"on"` 是为了现场手抄；
   但策略 id 打错**必须报错** —— 前者是格式差异，后者是语义错误。
5. **导出的"实际生效值"比"配置文件"更接近真相。**
   现场调参之后，配置文件已经不能反映系统真实状态了。

## 16.12 涉及文件

| 文件 | 变更 |
| --- | --- |
| `P1/**` | **新增**（5 个 src、1 个 test、1 个 data、3 个 script、1 个 docs） |
| `04/src/strategy_base.h` | `IStrategy` 增加只读 `params()`（配置导出用） |
| `04/src/strategy_manager.h` | `StrategyManager` 增加 `get_strategy(id)` |
| `08/src/dispatch_coordinator.h` | `DispatchCoordinator` 增加 5 个只读设备参数访问器 |
| `scripts/build_all.bat` | 20 步 → **21 步** |
| `README.md` | 目录树 + §2.1（21 步 / 7132 断言）+ §2.13（P1）+ 依赖图 + include 表 + 设计索引 |
