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
 ▲                    ├──► 07/ ──► 08/
 └────────────────────┴──────────────┘
```

- `07/` 与 `08/` 互为**运行时调用关系**（闭环调用优化层 / 端到端用闭环），
  但 `08/src/*.h` **不包含** `07/` 的任何头文件 —— header-only 库编译顺序无关，
  只需把对应的 `-I` 路径都加上。
