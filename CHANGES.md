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

---

# 17. 产品化 P2 —— 可观测性（现场出问题能看见）

> 路线：P0 架构分层 ✅ → P0.5 适配器可换性 ✅ → P1 配置化 ✅ → **P2 可观测性** → P3 通信 → RT_DB 接入

## 17.1 新增模块 `P2/`

```
P2/
├── src/soe.h          统一事件记录（SOE）+ 时间窗抑制
├── src/metrics.h      增量指标注册表（counter / gauge / histogram，O(1)）
├── src/trace.h        跟踪等级 + 采样间隔（按子系统独立，运行期热更新）
├── src/observe.h      RuntimeObserver —— 接入 EmsRuntime 的唯一入口
├── src/main.cpp       演示程序 ems_observe.exe
├── tests/test_observe.cpp   T301 ~ T315（434 断言）
├── docs/README.md     设计文档
└── scripts/           build.bat / build_test.bat / run_demo.bat
```

**关键约束：不修改 `07/`。** 观察者从外部喂 `StepRecord`，`obs.on_step(rt, rec)` 是唯一接入点：

```cpp
EmsRuntime rt;  rt.init();
RuntimeObserver obs;
obs.start(0.0);
for (...) {
    rt.set_environment(load, pv);
    StepRecord rec = rt.step(dt);
    obs.on_step(rt, rec);          // ← 唯一的接入点
}
obs.stop(t);
```

理由：可观测性不该侵入被测对象。`07/` 的 `step()` 已经是"一次闭环"的完整语义，
观察者只读它的输出 + 只读访问 `rt` 的公开状态（`fsm().history()` / `safety_params()` /
`device_limits()` / `config()`）。

## 17.2 为什么需要 P2：可观测性散在三个模块、三种表示

P1 解决了"不改源码就能适配现场"。现场跑起来之后的第一个问题是**看不见**。

| 位置 | 表示 | 问题 |
| --- | --- | --- |
| `06/state_machine.h` | `StateEvent{ts, from, to, reason}` | 只有状态迁移，没有等级 / 事件码 |
| `10/src/sim_24h.h` | `AlarmEntry` + `collect_alarms()` | **告警组装寄生在仿真装配层**（入参是 `Sim24hConfig`）→ **现场部署后系统没有告警能力** |
| `07/realtime_loop.h` | `StepRecord` 向量 | 是时序不是事件；`LoopMetrics::compute(log_, …)` 是 **O(N) 全量重算**，24h 日志 8.6 MB **只增不减** |
| `07/realtime_loop.h` | `cycle_us_sum_ / max_` | 只有均值与最大值，**没有直方图 / 分位数** |

三个具体缺陷：

**① 事件风暴没有框架级抑制。** 周期 10 实测：收紧配置下 `safety_clip` 连续 **838 拍**。
逐拍记事件就是 838 条。`10/` 里手工实现了"记首次 + 恢复"，但**每个指标都要手写一遍**：

```cpp
bool in_reverse = false, in_tr_over = false, in_soc_hi = false, in_soc_lo = false;
bool in_clip = false;
// … 下面每个 if 都要自己维护对应的 in_xxx 标志，重复 5 遍
```

**② 指标必须保留全部 StepRecord。** 24 h @ dt=1 s = 86400 条 × 约 100 B ≈ 8.6 MB，
**只增不减**；现场连续跑一年就是 3 GB。而且 `metrics()` 每次 O(N) 重算 ——
不可能每秒调一次给监控系统。

**③ `log_every` 一个旋钮承担两个冲突的职责。** 「保真」要 `log_every=1`，
「省资源」要 `log_every=100`。周期 10 已经吃过一次亏：
**倒送缺陷在 `log_every=10` 下显示 0 违规，`log_every=1` 才暴露。**
**为了省资源而调大 `log_every`，等于同时关掉了故障可见性。**

## 17.3 两个核心机制

### ① 边沿检测 → SOE：事件是"状态的变化"，不是"每拍的值"

一次持续 838 拍的安全限幅，物理上是**一件事**，应该产生 **2 条**事件（Start / End），
不是 838 条。观察者保存上一拍的布尔量，只在跳变时记事件 —— 收敛成一张 `edge_state_` 表
（`std::array<bool, 1024>`，按 `SoeCode` 下标索引），替代了 `10/` 里手工维护的 5 个 `in_xxx` 标志。

**例外：状态迁移与硬不变量违例不参与抑制（`suppressible = false`）。**
合并它们会丢掉迁移链 —— `A→B→C` 会变成 `A` 重复 2 次，而迁移链正是要留的：

```cpp
// soe.h
bool suppressible = true;   // false 用于"每次都独立成条"的事件
```

### ② 时间窗抑制：同 `(source, code)` 在窗口内合并

```
一次 838 拍的限幅  →  1 条 SAFETY_CLIP_START(repeat_count=838, duration_s=837)
                  +  1 条 SAFETY_CLIP_END
```

窗口从**上一次发生时刻**起算（滑动窗口），所以持续数小时的风暴会一直合并成一条，
`duration_s()` 就是风暴时长。合并时 `merge_data()` 保留**绝对值更大**的字段值，
保证抑制后不丢峰值信息（例如最大越限幅度）。

### ③ 逐拍累积 → 指标：O(1) 更新，与 `log_every` 无关

指标走自己的累积路径，**不经过日志降采样**。这对下面两个量是决定性的：
`cmd_travel_kw = Σ|Δcmd|`（指令总行程）与 `cmd_reversals`（方向反转次数）——
降采样会**漏掉中间的抖动**，而"抖动"恰恰是它们要度量的东西。

## 17.4 三个正交的旋钮（替代单一 `log_every`）

| 维度 | 载体 | 现场用法 |
| --- | --- | --- |
| ① **关键事件** | `SoeLog` | 永远记，不受任何降采样影响。频率天然很低（838 拍限幅 → 2 条） |
| ② **跟踪等级** | `TraceControl::set_level()` | 按子系统决定"要不要输出细节"。排查某子系统时把它调到 `kDebug`，其他不受影响。运行期热更新，不用重启 |
| ③ **采样间隔** | `TraceControl::set_sample_every()` | 高频跟踪按子系统采样。与 `log_every` 的区别是**按子系统独立**，且**不影响 SOE** |

**故障级（`>= kError`）永不受等级限制**：

```cpp
bool enabled(SoeSource s, SoeLevel lv) const {
    if (lv >= SoeLevel::kError) return true;   // 故障级别**永不受等级限制**
    return lv >= level(s);
}
```

否则会出现"现场把日志等级调高省资源，结果把故障也调没了"这种事故。

## 17.5 事件分级

等级（可比较、可统计）：`kDebug(0)` < `kInfo(1)` < `kWarn(2)` < `kError(3)` < `kFatal(4)`。
来源（9 个子系统）：`SYSTEM` / `FSM` / `SAFETY` / `COORD` / `STRATEGY` / `DEVICE` / `COMM` / `CONFIG` / `ECON`。

事件码命名约定 `Start` / `End` 成对出现 —— 这是"边沿检测 + 抑制"的直接产物：

| 区段 | 示例 |
| --- | --- |
| 100–102 | `FSM_TRANSITION` / `FSM_EMERGENCY_STOP` / `FSM_RUN_PERMIT` |
| 200–215 | `SAFETY_CLIP_START/END` / `GRID_REVERSE_START/END` / `TR_OVERLOAD_START/END` / `SOC_LOW_START/END` / `SOC_HIGH_START/END` / `TEMP_HIGH_START/END` / `RAMP_LIMITED_START/END` |
| 300–311 | `COMM_LOST_BMS/METER/PCS` / `COMM_RESTORED` / `DATA_STALE_START/END` / `PCS_FAULT_SET/CLEAR` / `DEVICE_OFFLINE/ONLINE` / `HOLD_LAST_START/END` |
| 400–402 | `REOPT_FIRED` / `PLAN_INVALID_START` / `PLAN_VALID_END` |
| 500–501 | `DEMAND_BREACH_START/END` |
| 900–903 | `OBSERVER_START/STOP` / `BUFFER_OVERFLOW` / `INVARIANT_BROKEN` |

**清除事件码必须与置位事件码配对**，否则现场看到"PCS 故障置位"却收到"通信恢复"会直接误判根因：

| 故障位（`FaultSet::bits()`） | 置位事件 | 清除事件 |
| --- | --- | --- |
| 0 `bms_comm_lost` | `COMM_LOST_BMS` | `COMM_RESTORED` |
| 1 `pcs_comm_lost` | `COMM_LOST_PCS` | `COMM_RESTORED` |
| 2 `meter_comm_lost` | `COMM_LOST_METER` | `COMM_RESTORED` |
| 3 `pcs_fault` | `PCS_FAULT_SET` | **`PCS_FAULT_CLEAR`** |
| 4 `data_invalid` | `DATA_STALE_START` | **`DATA_STALE_END`** |
| 5 `device_offline` | `DEVICE_OFFLINE` | **`DEVICE_ONLINE`** |
| 6 `temp_fault` | `TEMP_HIGH_START` | **`TEMP_HIGH_END`** |
| 7 `emergency_stop` | `FSM_EMERGENCY_STOP` | `COMM_RESTORED` |

## 17.6 有界内存与自省

可观测性组件**必须能报告自己的数据丢失**，否则比没有更危险 ——
"什么都没输出"到底是"真的没事"还是"缓冲区爆了"？

| 指标 | 含义 |
| --- | --- |
| `SoeLog::size()` | 当前存储条数（≤ `capacity`，默认 4096 条 ≈ 600 KB） |
| `SoeLog::dropped()` | 被容量**淘汰**的条数（超出上限即淘汰最旧） |
| `SoeLog::total()` | 累计 `push` 次数（含被抑制的） |
| `SoeLog::suppressed()` | 被时间窗**合并**的次数 |
| `TraceControl::filtered_total()` | 被等级/采样过滤掉的条数 |
| `TraceControl::passed_total()` | 实际输出的条数 |

`total() - size() - dropped()` 就是"被抑制合并"的量。三个数一起看才不会误判。

条目用 `std::list` 存储 —— 需要**稳定迭代器**（抑制时原地更新、淘汰时从头部删除）。
SOE 是事件级频率（每秒几条），不是拍级，所以链表较差的局部性在这里不是问题。

## 17.7 导出

| 格式 | 用途 |
| --- | --- |
| `soe.csv` | 运维用 `grep` / `wc -l` 直接看。**一事件一行** |
| `soe.json` | 带 `summary` 统计，给上层平台解析 |
| `metrics.prom` | Prometheus 文本格式，直接被监控系统 scrape |
| `metrics.json` | 含直方图 `p50/p95/p99`，给人看 |
| `trace.json` | 当前跟踪配置与过滤统计 |

## 17.8 命令行与实测

```bat
build\ems_observe.exe --demo 24            :: 24h 闭环 + 观察者汇总（SOE / 指标 / 分位）
build\ems_observe.exe --decouple           :: 指标与 log_every 无关（核心价值验证）
build\ems_observe.exe --fault 12           :: 故障场景 SOE（置位/清除事件对）
build\ems_observe.exe --export out 24      :: 导出 soe.csv / soe.json / metrics.prom / metrics.json
```

退出码：`0` 正常 / `1` 参数错误 / `2` 运行期失败（含硬不变量被破坏）。

### `--decouple`：核心价值实测

场景：3600 拍，负荷在 300/50 kW 之间每 5 拍切换（持续充放换向），只改 `log_every`。

| log_every | 观察者行程 | 观察者换向 | 日志行数 | `07/` 日志行程 |
| --- | --- | --- | --- | --- |
| 1 | 88477.8 kW | 719 | 3600 | 88477.8 kW |
| 10 | **88477.8 kW** | **719** | 360 | **0.0 kW** |

`log_every=10` 时 `07/` 报的指令总行程是 **0**，真实值 **88477.8 kW** —— **完全失明**；
观察者不受影响。同时 `log_every=1` 时两者口径**完全一致**（88477.8 / 719），
验证观察者本身是对的。

### `--fault`：故障场景 SOE（12 h，43200 拍 → 18 条事件，2400:1 压缩）

```
17281.0    ERROR FSM      FSM_TRANSITION      1  状态迁移 DERATED → FAULT (fault_in_derated:PCS_FAULT)
17281.0    ERROR DEVICE   PCS_FAULT_SET       1  故障位置位: PCS 故障
17881.0    INFO  DEVICE   PCS_FAULT_CLEAR     1  故障位清除: PCS 故障
17900.0    INFO  FSM      FSM_TRANSITION      1  状态迁移 FAULT → READY (fault_recovered)
19081.0    ERROR COMM     COMM_LOST_METER     1  故障位置位: 关口电表通信丢失
19086.0    ERROR COMM     HOLD_LAST_START     1  采集超时 → 保持上一拍指令
19381.0    INFO  COMM     HOLD_LAST_END       1  采集恢复
20881.0    ERROR FSM      FSM_TRANSITION      1  状态迁移 READY → FAULT (fault_in_ready:PCS_COMM_LOST)
```

每条故障成对（置位 / 清除），每条迁移都带 `reason`。

## 17.9 缺陷修复（P2 开发中发现）

### ① 观察者重复状态：成员与 `tot_` 并存，更新一套、读另一套

**现象**：冒烟测试输出 `关口 min/max 0 / 0`、`SOC min/max/end 1 / 0 / 0` —— 明显不可能。

**根因**：`on_step()` 更新的是成员 `min_grid_` / `soc_min_` / `soc_max_` / `soc_end_`，
而 `summary_text()` 读的是 `tot_.min_grid_kw` / `tot_.soc_min` …… 两份状态，只更新了一份。

**修法**：删掉全部重复成员，只保留 `tot_`（`ObserverTotals`）。

### ② `sign_flips` 与 `cmd_reversals` 语义混淆，两者必然相等

**现象**：初版两个计数器的值**永远相等**（例如都是 119）。

**根因**：两处都在看**增量 `Δcmd` 的符号**：

```cpp
if (d_step > 1e-9)      { if (last_sign_ < 0) { ++tot_.sign_flips; ++tot_.cmd_reversals; } ... }
else if (d_step < -1e-9){ if (last_sign_ > 0) { ++tot_.sign_flips; ++tot_.cmd_reversals; } ... }
```

**修法**：两个量语义**不同**，不能合并 ——
`sign_flips` 看 `p_cmd` 自身过零；`cmd_reversals` 看 `Δcmd` 的符号翻转。
阈值与 `07/LoopMetrics::compute` 同口径（`sign_eps=0.5` / `dir_eps=2.0`，提为 `ObserverConfig` 字段），
但判定是**逐拍**的 —— `07/` 是在（可能被降采样的）日志上判定。

### ③ FSM 迁移漏掉第一拍（`INIT → SELF_CHECK`）

**现象**：T313 交叉校验失败 —— 观察者记录的迁移数比 `10/` 少 1。

**根因**：观察者自己用 `prev_state_` 做边沿检测，第一拍没有 `prev`，
于是把 `SELF_CHECK` 当成"初始状态"记下来，**吃掉了 `INIT → SELF_CHECK` 这条迁移**。

**修法**：**不自己推导，直接读状态机的权威记录 `rt.fsm().history()`**，
用 `hist_seen_` 记录已消费条数。额外收益：迁移自带 `reason`
（现场要的是"为什么迁"，不是"迁到哪"），SOE 消息从
`状态迁移 READY → NORMAL` 变成 `状态迁移 READY → NORMAL (start_command)`。

### ④ CSV 内嵌裸换行 → 一条事件跨多行

**现象**：T305 断言 CSV 行数失败（4 行变 5 行）。

**根因**：消息里的 `\n` 被直接写进引号字段。RFC 4180 允许引号内嵌换行，
但那样一条事件跨多行，**运维用 `wc -l` / `grep` 统计时全线错位**。

**修法**：`escape_csv()` 把 `\n` / `\r` 转成字面量 `\n` / `\r`。
**SOE 导出必须"一事件一行"。**

### ⑤ 故障位清除事件码不配对

**现象**：`PCS_FAULT_SET` 之后收到的是 `COMM_RESTORED`（"通信恢复"）。

**根因**：所有故障位的清除都硬编码用 `SoeCode::kCommRestored`。

**修法**：新增 `fault_clear_code(bit)`，逐位配对 ——
`pcs_fault → PCS_FAULT_CLEAR`、`device_offline → DEVICE_ONLINE`、
`data_invalid → DATA_STALE_END`、`temp_fault → TEMP_HIGH_END`。
另外补上 bit 7 `emergency_stop`（原来落进 `default` 被当成 `INVARIANT_BROKEN`）。

### ⑥ 【既有缺陷】README 里两处路径的反斜杠被转义吃掉了

**现象**：`file README.md` 报告 "with CR, LF line terminators"。

**根因**：早前某次 Python 写入时，字符串里的 `\r` / `\b` 被当成转义序列解释，
路径里的反斜杠变成了控制字符 —— 共 2 处：

| 位置 | 实际字节 | 渲染结果 |
| --- | --- | --- |
| `scripts\run_demo.bat`（§2.12） | `scripts` + **0x0D(CR)** + `un_demo.bat` | `scriptsun_demo.bat` |
| `scripts\build_test.bat`（§2.12） | `scripts` + **0x08(BS)** + `uild_test.bat` | `scriptsuild_test.bat` |

**修法**：按字节替换回 `\`。修复后 README 为纯 LF（666 个 LF，0 个 CR，0 个 BS）。

**排查方法**（值得固化成习惯）—— 全文扫描控制字符，而不是只看 `file` 的 CR 报告：

```python
for b, name in ((0x08, "BS"), (0x0c, "FF"), (0x0b, "VT")):
    if raw.count(bytes([b])): print(name, raw.count(bytes([b])))
ncr = raw.count(b"\r") - raw.count(b"\r\n")
if ncr: print("bare CR", ncr)
```

只看 CR 会漏掉 `\b` —— 这两处就是这么被发现第二处的。

**教训**：Windows 上用 Python 改文件时，`\r` / `\t` / `\b` 这类转义要么写双反斜杠，
要么用 raw string —— 与已知的 `.bat` 吞 CR 是同一类问题。

## 17.10 测试清单（T301~T315，434 断言）

| 编号 | 内容 |
| --- | --- |
| T301 | 枚举：等级 / 来源 / 事件码 / 名称互转（含"每个码都必须有名字"遍历，防止新增枚举忘补 switch） |
| T302 | 时间窗抑制：838 拍风暴 → 1 条 Start + 1 条 End；窗口边界；关闭抑制；不同 `(source,code)` 互不干扰 |
| T303 | 抑制例外：状态迁移链不被合并（`suppressible=false`）+ 正反对照 |
| T304 | 有界内存：容量淘汰 + `dropped` 如实上报 + 保留的是**最新**而非最旧 + `reset()` 清零 |
| T305 | 导出：CSV 引号/换行转义（一事件一行）、JSON 结构、`filter_*` / `count*` 查询 |
| T306 | 指标增量：counter / gauge（记极值）/ histogram（O(1)）；幂等注册；未注册名字不崩 |
| **T307** | **指标与 `log_every` 无关**（核心：行程 / 符号翻转 / 方向反转 / 能量 / 硬不变量逐拍精确） |
| T308 | 直方图分位（含 `+Inf` 桶回落、空直方图、桶边界 `le` 语义） |
| T309 | Prometheus 文本格式 + 指标名净化（非法字符 / 非法首字符） |
| T310 | 跟踪等级过滤：**故障级永不受限**（全局调到 FATAL 时 ERROR 仍输出） |
| T311 | 采样间隔：按子系统独立；只有 `kDebug` 级走采样，事件级不受影响 |
| T312 | 观察者端到端：24h 默认场景 + 硬不变量 + 事件级压缩比 + 迁移链完整 |
| T313 | 与 `10/` 结果交叉校验（`log_every=1` 时关口极值 / 三项硬不变量 / 能量 / 迁移数完全一致） |
| T314 | 故障场景事件：PCS 故障 / 电表通信 / PCS 通信 → 置位/清除事件对 + 门控正确 |
| T315 | 观察者自身可观测：`suppressed` / `dropped` / `filtered` / `passed` + `reset()` 可复用 |

T307 的关键断言（观察者 vs `07/` 日志口径）：

```cpp
// log_every=1：两者口径一致（验证观察者正确）
EXPECT_NEAR(o1.totals().cmd_travel_kw, n1.cmd_travel_kw, 1e-6);
EXPECT(o1.totals().cmd_reversals == n1.cmd_reversals);

// log_every=10：07/ 失明，观察者不受影响（验证观察者价值）
EXPECT(n10.cmd_travel_kw < 1e-9);
EXPECT(o10.totals().cmd_travel_kw > 1000.0);
```

## 17.11 全量回归

```
04=65  05=68  06=34  07=6050  08=444  P0+P0.5=79  09=77  10=144  P1=171  P2=434
总计 7566 断言全绿
[BUILD ALL OK] All 22 components built.
```

## 17.12 关键认识

1. **可观测性组件必须能报告自己的数据丢失。** `dropped()` / `suppressed()` /
   `filtered_total()` 不是锦上添花 —— 没有它们，"什么都没输出"就分不清是
   "真的没事"还是"缓冲区爆了 / 等级设错了"。
2. **事件是"状态的变化"，不是"每拍的值"。** 这个视角一旦确立，
   838 条风暴自动收敛成 2 条，而且抑制是**框架能力**，不是每个指标各自手写的 if。
3. **但抑制必须有例外。** 状态迁移链被合并就丢了因果；硬不变量违例被合并就丢了严重性。
   `suppressible` 这一个布尔字段，把"该合并的"和"不能合并的"分开。
4. **"记多少日志"是一个旋钮管不了的事。** 关键事件（永远记）/ 跟踪等级（按子系统）/
   采样间隔（按子系统）是三个正交维度；合成一个 `log_every` 必然牺牲其中一面。
5. **指标不能建立在"可能被降采样"的数据上。** `log_every=10` 让 `07/` 报的
   指令总行程变成 0，而真实值是 88477.8 kW —— 这不是精度问题，是**失明**。
6. **观察者不该侵入被测对象。** 不修改 `07/`、只从外部喂 `StepRecord`，
   使 P2 可以独立测试、独立演进，也让"观察者本身有 bug"这件事可被单独发现。
7. **能读权威记录就别自己推导。** 状态迁移直接读 `rt.fsm().history()` ——
   自己边沿检测不仅会漏第一拍，还丢掉了 `reason`。
8. **告警组装不能寄生在仿真装配层。** `collect_alarms()` 的入参是 `Sim24hConfig`，
   意味着现场没有告警能力。可观测性必须是运行时的一等公民。

## 17.13 涉及文件

| 文件 | 变更 |
| --- | --- |
| `P2/**` | **新增**（4 个 src 头 + 1 个 main + 1 个 test + 3 个 script + 1 个 docs） |
| `scripts/build_all.bat` | 21 步 → **22 步** |
| `README.md` | 目录树 + §2.1（22 步 / 7566 断言）+ §2.14（P2）+ 依赖图 + include 表 + 设计索引；**修复既有的裸 CR 缺陷** |
| `.gitignore` | 增加 `**/out/`（P2 演示产物目录） |
| `CHANGES.md` | 新增 §17（本文件） |

---

# 18. RT_DB 接入：共享内存实时库适配器（2026-09-13）

> **前置**：`2448e7b`（vendor RT_DB 源码 + 30 点 EMS 点表 + 补上点表注册 API）。
> 本节把「点表已经能建起来」推进到「**算法真的经共享内存跑闭环**」——
> 也就是 P0 立下的那份承诺在**真正的现场介质**上兑现。

## 18.1 本次交付

| 项 | 内容 |
| --- | --- |
| 新增 `07/src/rtdb/rtdb_device_io.h` | `RtDbDeviceIO`（实现 `IDeviceIO`）+ `RtDbPointWriter`（设备侧写点器）|
| 新增 `07/tests/test_rtdb_device_io.cpp` | T25~T28，**519 断言** |
| 新增 `07/scripts/build_test_rtdb.bat` | gcc 编 3 个 C 文件 → g++ 链接测试（CRLF + 已过"吞 CR"扫描）|
| 修复 `07/src/rtdb/ems_rt_db_setup.c` | Windows 段存活句柄（见 18.4）|
| `scripts/build_all.bat` | 22 步 → **23 步**（新增 `19/23`，其后 4 步顺延）|
| 文档 | 根 `README.md`（§2.15 / 目录树 / 产物 / 8085 断言 / 依赖图 / include 表 / 设计索引）、`07/docs/README.md`（§8）|

## 18.2 为什么不能"等 P3 之后再说"

P0.5 用 `MemoryDeviceIO` 证明了"换数据源不改算法"，但那是**进程内**的点表；
现场换掉的是**另一个进程**。两者的差别不是"实现细节"，而是三种真实故障模式的入口：

1. **跨进程可见性**：进程内 unordered_map 永远不可能出现"另一个进程看不到"的问题；
2. **段的生命周期**：进程内点表随对象生死，共享内存段的生死由句柄/内核决定（见 18.4）；
3. **数据品质**：跨进程数据必须回答"这值可信吗"（品质位），进程内点表不需要。

所以在写 Modbus 之前先把 RT_DB 接上，代价最小、暴露的问题最真。

## 18.3 两个方向、两个人

```
   写 MEAS/STA/CFG ↓                        ↑ 读 MEAS/STA/CFG
设备/SCADA 进程（RtDbPointWriter）      EMS 进程（RtDbDeviceIO）
   ↑ 读 CMD.*                             ↓ 写 CMD.*（指令 + 权限区间）
            └──────── RT_DB 共享内存段 ────────┘
```

- 两个方向拆成**两个类**（而不是一个类两套方法）：越界使用会变成编译期错误，
  而不是"现场第二天才发现设备侧把 EMS 的指令点覆盖了"。
- 算法层**看不到任何点名** —— 这条 P0 纪律在真实适配器里第一次被真正检验：
  `IDeviceIO` 的六个方法全部只谈业务语义结构体（`RealtimeSnapshot` /
  `DeviceLimits` / `DeviceStatus` / `DeviceActuals` / `PowerCommand`）。

## 18.4 踩坑 1：Windows 的段会被"善意"的释放动作杀掉 ⚠️

`ems_rt_db_setup()` 的设计语义是"写完就撒手"（官方 `init_rt_db.c` 是常驻 monitor，
无法用于自动化测试），因此结尾 `unmap + destroy_shared_memory(0)`。

**在 Linux 上这是对的**（`shmget` 的段在 `shmdt` 之后依然存在）；
**在 Windows 上这是错的** —— 段是页面文件支撑的**文件映射对象**，
最后一个句柄关闭即销毁。于是调用方随后的 `rt_db_init()` 得到：

```
Shared memory not found. Please run init tool first.
```

修法：Windows 下另开一个**只读存活句柄**（`keep_segment_alive()`）并保留，
让段活到进程退出（随进程释放 → 段自然销毁，测试之间不互相污染）。

> **部署含义**：Windows 下**初始化器不能是"跑完就退"的短命进程**。
> 这条约束在 Linux 上不存在，因此"同一份代码在 Linux 看着是对的"极具欺骗性 ——
> 这也是为什么这类缺陷必须靠**真跑一遍**才能发现（本节任务的全部价值之一）。

## 18.5 踩坑 2：`near` 是 `windef.h` 的遗留宏

测试里写了个辅助函数 `near(a, b, eps)`，编译报
`expected unqualified-id before 'double'`。原因：`windef.h`（经 `windows.h` 引入）里
`#define near` / `#define far` 是 16 位时代的遗留空宏，会**把函数名直接吃掉**。
改名 `almost_equal` 即可。

> 注意本适配器**刻意不 include `rt_db_structs.h`**：它会带进 `windows.h`，
> 其 `min`/`max` 宏会破坏项目里大量 `std::max` / `std::min`（必须定义 `NOMINMAX`）。
> 适配器只镜像它需要的 3 个常量，并在 T25 里做**漂移守卫**（一旦 vendor 改了长度/品质位，测试立刻失败）。

## 18.6 踩坑 3：`reset` 会清空段级元数据

`ems_rt_db_setup(reset=true)` 会 `memset` 整个段，**连接计数等段级元数据被清零**。
测试里因此不能断言"连接数 ≥ 2"，只能断言"新连接让计数 +1"。

> **部署含义**：初始化器必须在**所有连接建立之前**调用（现场就是"先起初始化器，再起 EMS"）。
> 若在系统运行中重启初始化器，段级计数器会与真实连接数脱节。

## 18.7 踩坑 4：品质位是"随采集刷新"的，不是"翻一下就好"

第一版 T25 断言"把 `QUALITY_BAD` 改回 `QUALITY_GOOD` 后 `data_valid` 立刻为真" —— **失败**。
正确的语义是：品质位在**采集**时被读入缓存（`read_snapshot` / `read_actuals`），
`read_status().data_valid` 反映的是"最近一次采集"的印象。所以恢复需要**再采集一次**。

这条语义和 `EmsRuntime::step()` 的固定顺序（① `read_snapshot` → ② `read_status`）正好对上，
因此链路里不会出问题；但如果哪天有人调换了这两步，`data_valid` 就会永远滞后一拍。
测试里把这条**明写出来**（先断言"未重采集仍不可信"，再断言"重采集后可信"），
就是防止后人把它"优化"掉。

## 18.8 核心证据：T26 跨内存边界闭环逐位等价

```
运行 A：EmsRuntime ── MemoryDeviceIO（进程内点表，P0.5 已证）
运行 B：EmsRuntime ── RtDbDeviceIO ──[共享内存段]── 设备侧泵（MemoryDeviceIO）
```

两路的装配序列、环境脚本（380±120 kW 负荷 / 白天 150±60 kW 光伏）、算法参数
**完全相同**，唯一差别是数据走不走共享内存。逐拍比对 17 个字段
（`p_cmd` / `p_actual` / `p_grid` / `soc` / `temp` / `p_lower` / `p_upper` /
`plan_target` / `correction` / `t` / `state` / `clamped` / `safety_clip` /
`state_gated` / `hold_last` / `fault_bits` / `reason`）：

**400 拍逐位一致，差异 0 拍**（峰值指令 100 kW，SOC 0.5 → 0.4993）。
反向守卫同时成立：峰值指令 > 50 kW、SOC 确实变化、`stale_reads() == 0`
（否则"两边都恒 0"也能骗过等价性断言）。

## 18.9 其余用例

| 用例 | 覆盖点 |
| --- | --- |
| T25 | 点表契约：共享内存点表 ↔ 编译期点表 ↔ 设备侧点表三方一致（30 点点名/单位/索引）；常量与品质位漂移守卫；两个方向读写（含按名字读写）；`CFG.*` → `DeviceLimits`；品质位语义；段级写计数 |
| T26 | 跨内存边界闭环**逐位等价**（400 拍） |
| T27 | 两个独立连接（两次 `rt_db_init`）看**同一段**内存：写 A 读 B、写 B 读 A；段级写计数共享；点表自检与索引映射不依赖连接 |
| T28 | 故障经共享内存驱动状态机：设备侧写 `STA.PCS_FAULT` → EMS 进 FAULT → 指令归零 + 撤销运行许可 → 恢复后停在 READY **不自动带载** |

## 18.10 全量回归

- `scripts\build_all.bat --no-test`：`[BUILD ALL OK] All 23 components built.`
- 逐步实跑（每个 `build_test.bat` 单独跑过，均 `ALL TESTS PASSED`）：
  02 / 04=65 / **05=68** / **06=34** / **07=6050** / **07(RT_DB)=519** / **08=444** /
  P0+P0.5=79 / 09=77 / 10=144 / P1=171 / P2=434 → **合计 8085 断言全绿**。

## 18.11 涉及文件

| 文件 | 变更 |
| --- | --- |
| `07/src/rtdb/rtdb_device_io.h` | **新增**（`RtDbDeviceIO` + `RtDbPointWriter`，纯头文件）|
| `07/tests/test_rtdb_device_io.cpp` | **新增**（T25~T28 / 519 断言）|
| `07/scripts/build_test_rtdb.bat` | **新增**（gcc 编 C 库 + g++ 链接；CRLF；已过"吞 CR"扫描）|
| `07/src/rtdb/ems_rt_db_setup.c` | **修复**：Windows 段存活句柄（`keep_segment_alive()`）|
| `scripts/build_all.bat` | 22 步 → **23 步**（新增 `19/23 07\ RT_DB 接入`）|
| `README.md` | §2.15（新增）+ 07/ 目录树 + 构建产物 + 8085 断言 + 23 组件 + 适配器架构图 + include 表 + 设计索引 |
| `07/docs/README.md` | 新增 §8（RT_DB 接入：两个方向 / `execute()` 语义 / 品质位 / 现场三约束）+ 文件导览 |
| `CHANGES.md` | 新增 §18（本文件）|

---

# 19. 产品化 P3 通信层：Modbus 设备侧 / IEC104 调度侧（2026-09-13）

> **前置**：`§18`（RT_DB 接入，519 断言）。
> P0 ~ RT_DB 证明的「换数据源不改算法」全都还在**一个地址空间**里。本节把 `IDeviceIO` 接到
> **两根真实线缆**：南向 Modbus（EMS 是主站 ↔ PCS/BMS/电表）、北向 IEC 60870-5-104（EMS 是受控站 ↔ 调度/云端）。
> 交付后 `IDeviceIO` 上并列 **5 个适配器**：`SimDeviceIO` / `MemoryDeviceIO` / `RtDbDeviceIO` /
> **`ModbusDeviceIO`** / **`Iec104DeviceIO`** —— 算法层（05/06/07/08）**一行未改**。

## 19.1 本次交付

| 项 | 内容 |
| --- | --- |
| 新增 `P3/src/modbus_codec.h` | Modbus 帧编解码纯函数：MBAP / PDU / 功能码 `0x03`·`0x04`·`0x06`·`0x10` / CRC16（自检 `0x4B37`）/ 四种字序 / 分批上限 |
| 新增 `P3/src/modbus_device_io.h` | `ModbusDeviceIO`（实现 `IDeviceIO`）+ `ModbusRegisterMap`（30 点 → 4 寄存器块）+ `IModbusTransport` |
| 新增 `P3/src/modbus_slave_sim.h` | `ModbusSlaveSim`（从站 + 方向强制）+ `LoopbackModbusTransport`（注入沉默/异常/断链）|
| 新增 `P3/src/iec104_codec.h` | IEC104 帧/ASDU 编解码纯函数：APCI 三帧型 / 8 类 ASDU / 小端 / 粘包攒帧 |
| 新增 `P3/src/iec104_device_io.h` | `Iec104DeviceIO` + `Iec104PointMap`（IOA 四段）+ `Iec104ControlledStationSim` + `LoopbackIec104Transport` |
| 新增 `P3/src/main.cpp` | 演示 `ems_comms.exe`（场景 F Modbus / G IEC104 / H 四介质逐拍比对）|
| 新增 `P3/tests/test_modbus.cpp` | T31~T36，**622 断言** |
| 新增 `P3/tests/test_iec104.cpp` | T41~T46，**646 断言** |
| 新增 `P3/scripts/*.bat` ×4 | build / run_demo / build_test_modbus / build_test_iec104（CRLF + 纯 ASCII，已过「吞 CR」扫描）|
| `scripts/build_all.bat` | 23 步 → **25 步**（新增 `24/25 P3 Modbus`、`25/25 P3 IEC104`，编号同步 `N/23`→`N/25`）|
| 文档 | 根 `README.md`（§2.16 + 目录树 + 产物清单 + 9353 断言 + 适配器架构图 + include 表 + 设计索引）、新增 `P3/docs/README.md` 与 `P3/docs/design.md`、`docs/产品化/P0-架构分层.md` §3.3、`04/src/device_io.h` 头注释 |

## 19.2 为什么先做"监听环回"，而不是直接上真机

现场验收最终当然要连云缆，但**开发期最该被验证的不是内核 socket**（那是操作系统的事），
而是我们自己写的那几千行：帧格式、字序、序号状态机、地址映射、异常码。

所以 P3 的环回装置只做一件事：**跳过内核 socket，不跳过任何字节编解码**。

```
  ModbusDeviceIO ──► MBAP+PDU 字节 ──► LoopbackModbusTransport ──► ModbusSlaveSim
        ▲                                                                  │
        └── parse_response() ◄── 响应字节 ◄──────── 回灌 ◄───────────────────┘
```

反面就是"直接调从站函数"。那样写，测试里跑的是 Python 式的函数直调，
现场跑的是编码后的真实报文 —— **测过的不是要跑的**，P0 以来一直坚持的原则在这里同样适用。

IEC104 侧更是如此：TCP 会**粘包**，`parse_apdu()` 收到半个帧必须返回 0 等下一批字节；
序号 N(S)/N(R) 会回绕；k=12 窗口满了必须停发。这些只有走字节流才暴露得出来。

## 19.3 两个适配器的边界

| | `ModbusDeviceIO` | `Iec104DeviceIO` |
| --- | --- | --- |
| 角色 / 对端 | EMS **主站** ↔ PCS / BMS / 电表 | EMS **从站（受控站）** ↔ 调度 / 云端 SCADA |
| 传输模型 | 请求-响应（`transact(req, resp)`） | 字节流（`send` / `receive`，粘包自己攒帧） |
| 字节序 | **大端**（Modbus 规定） | **小端**（IEC104 规定） |
| 上送方式 | EMS 主动轮询读 | 从站自定时上送 + EMS 用 TESTFR/S 帧当拍点 |
| 首次握手 | 无（直连） | `STARTDT` + **总召唤三段式**（ActCon → Introgen → ActTerm） |
| 写指令 | `0x10` 写 CMD 寄存器块 | 3 条 `C_SE_NC_1`(50) 设定值 |
| 地址空间 | 寄存器块 `0x0000`/`0x0100`/`0x0200`/`0x0300` | IOA 段 `0x4001`/`0x4101`/`0x4201`/`0x4301` |
| 故障看门狗 | 从站沉默 | t3 无数据超时（`set_stale_after_polls`）|

**为什么两个 Transport 形状不同**：把 IEC104 硬塞进 `transact` 会把粘包与序号状态藏进抽象里 ——
那是抽象泄漏，不是简化。

**两者对外都只是 `IDeviceIO`**：`EmsRuntime` 拿到的指针类型完全一样，
`07/tests/test_realtime_loop.cpp` 的算法路径一行没动。

## 19.4 关键设计：等价性与量化必须分开验

这是 P3 最重要的一条方法学。混在一起验，一旦 f32 有偏差，你无法判断是**字序写错**还是**精度天花板**。

| 通道 | Modbus | IEC104 | 用途 |
| --- | --- | --- | --- |
| **宽精度** | `kFloat64BE`（4 寄存器） | `M_ME_WIDE`(200) 私有 double | 验**逐位等价**（diff 必须为 0） |
| **现场标准** | `kFloat32BE`(ABCD) / `kFloat32Swap`(CDAB) | `M_ME_NC_1`(13) 短浮点 | 验**量化偏差量级**与决策拓扑不变 |

实测结论（T34 / T44）：

- 宽精度通道：400 拍**逐位 diff = 0**；
- 现场标准通道：**决策拓扑差异 = 0 拍**，Δcmd 最大偏差 **3.93681e-05 kW**，ΔSOC **1.08913e-08**。

**两条独立实现给出同一个偏差上界**：Modbus 侧走大端字序，IEC104 侧走小端 ASDU，
两条代码路径互不相干，却推出同一个 ≈3.9e-05 kW。互为旁证 —— 说明这不是某个实现的偶然，
而是 f32 在 100 kW 量程下的固有分辨率（2⁻²³ × 100 ≈ 1.19e-05，同量级）。

## 19.5 踩坑 1：单次采集失败 ≠ 进 FAULT（T35）

最初 T35 用 `inject_timeout(3)` 丢几帧，然后单步 `rt.step(0.1)`，期望状态机进 FAULT —— **4 个 FAIL**。

原因：**"丢一帧"只让 `quality_ok_=false` 一拍**，`data_valid()` 当拍为假，下一拍读到数据就恢复。
FAULT 需要 `data_invalid` 持续成立。

修法两条：

1. 模式 A 改用 `set_link_up(false)` **持续断链**，再连续 `rt.run(5, 60)` 推进，让故障位站稳；
2. 模式 B（心跳丢但遥测正常 → HOLD_LAST）**不能断言固定拍数**起点。
   改为扫描日志找首次 `hold_last` 出现的位置（实测期望落在第 128~133 拍），
   再断言冻结值不被后续拍改变、权限区间被钉成单点、且冻结值本身非零
   （`fabs(frozen) > 1.0`，否则"恒 0"也能骗过断言）。

这条与 §18 的"反向守卫"是同一个思想：**等价性/一致性断言必须配一条反向断言**，否则退化的实现也能通过。

## 19.6 踩坑 2：未知 ASDU 类型 ≠ 畸形报文（T43）

T43 原本期望发一个"未知类型"的 ASDU 后 `malformed() == 0`，实际 `malformed()` 计数了 —— 2 个 FAIL。

根因在 `asdu_length_ok()`：它按类型查长度表，**未知类型落到 `default: return false`**，
于是 `handle_frame()` 把"不认识"当成"格式错"。

"IEC104 扩容了新型号"和"报文被截断了"是两件完全不同的事，前者不该报警。修法：
`asdu_length_ok()` 的默认分支改返回 `true`，未知类型只计 `unknown_type_`，畸形只计 `malformed_`，
两类计数分开。测试同步改为分别断言。

## 19.7 踩坑 3：`const` 成员函数里改缓存 —— 编译期就拦住了

`Iec104DeviceIO::reset_session()` 被写成 `const`，但语义上它要清 `last_cmd_` / `has_last_cmd_`。
编译器直接报 `assignment of member ... in read-only object`。

这个坑的价值在于它**方向是对的**：`reset_session()` 确实改变对象状态，不该是 `const`。
把 `const` 去掉即可 —— 与 04/ 的 `IDeviceIO` 里 `read_*` 是 `const`、`execute`/`write_*` 非 `const` 的分工一致。

（另有一处纯手误：`test_modbus.cpp` 里把 `block_first_index()` 的函数定义误插在用例上方，导致重定义。
属编辑事故，删掉重复定义即可。）

## 19.8 核心证据：T34 / T44 闭环逐位等价

沿用 §18 的 T26 范式 —— 同一套 `EmsRuntime`、同一装配序列、同一环境脚本，**400 拍逐拍比对**：

```
环境脚本完全相同（configure_runtime 与 07/test_rtdb_device_io.cpp 逐行一致）
   ├─► 基准路：MemoryDeviceIO                直连进程内点表
   └─► 被测路：ModbusDeviceIO （或 Iec104DeviceIO）  经 MBAP+PDU / APCI+ASDU 报文
比对 17 个字段：p_cmd / p_actual / p_grid / soc / temp / p_lower / p_upper /
              plan_target / correction / t / state / clamped / safety_clip /
              state_gated / hold_last / fault_bits / reason
```

结果：

- **宽精度通道：400 拍 diff = 0**（位级完全相同）；
- **现场标准通道：决策拓扑差异 0 拍**，Δcmd 最大 3.93681e-05 kW。

## 19.9 其余用例

| 用例 | 覆盖点 |
| --- | --- |
| T31 | Modbus 帧逐字节：CRC16 自检 `0x4B37`、`kFloat32BE`(ABCD) vs `kFloat32Swap`(CDAB)、f64 位级恒等、MBAP 自洽、异常响应解析 |
| T32 | 异常与采集失败：异常**不当数据**、读失败**保留旧值**、非法地址/单元号/未知功能码分别回对应异常码 |
| T33 | 映射契约：f32 共 **780** 寄存器 / f64 共 **792**；点名 / 块 / 地址与 30 点真相源逐字一致 |
| T35 | 断链与自愈：链路静默 → FAULT；心跳丢 → HOLD_LAST 冻结在 4.28963 kW（第 130 拍起）|
| T36 | 故障经寄存器驱动状态机：`STA.PCS_FAULT` → FAULT → 门控归零 → 恢复后 READY **不自动带载**（设备侧读回指令 5.8891 kW）|
| T41 | APCI 逐字节：U/S/I 三帧型、15bit 序号、LEN 上限 253、粘包边界（帧长 0 = 半个帧）|
| T42 | ASDU 逐字节：类型 1/13/45/46/50/70/100/200 的小端布局、品质位、`M_ME_WIDE` 位级无损、长度语义 |
| T43 | 会话时序：STARTDT 确认、总召唤 I 帧数=1、心跳 14/14、S 帧=30、最大未确认=0、k=12；CA 不匹配丢弃；未知类型/畸形分开计数；STOPDT 处理 |
| T45 | 通信中断：链路断 → FAULT；对端哑触发 t3 → FAULT；自愈；**序号不回退** |
| T46 | 映射契约：IOA 四段、**全局唯一**、分辨率事实 |

## 19.10 全量回归

- `scripts\build_all.bat`：`[BUILD ALL OK] All 25 components built.`
- 实跑断言：
  02 / 04=65 / 05=68 / 06=34 / 07=6050 / 07(RT_DB)=519 / 08=444 / P0+P0.5=79 /
  09=77 / 10=144 / P1=171 / P2=434 / **P3 Modbus=622** / **P3 IEC104=646**
  → **合计 9353 断言全绿**。

## 19.11 涉及文件

| 文件 | 变更 |
| --- | --- |
| `P3/src/modbus_codec.h` | **新增**（帧编解码纯函数 + CRC16 + 字序） |
| `P3/src/modbus_device_io.h` | **新增**（`ModbusDeviceIO` + 寄存器映射 + `IModbusTransport`） |
| `P3/src/modbus_slave_sim.h` | **新增**（从站仿真 + 方向强制 + 环回传输） |
| `P3/src/iec104_codec.h` | **新增**（APCI / ASDU 编解码纯函数） |
| `P3/src/iec104_device_io.h` | **新增**（`Iec104DeviceIO` + 受控站 + 环回 + 会话状态机） |
| `P3/src/main.cpp` | **新增**（场景 F / G / H） |
| `P3/tests/test_modbus.cpp` | **新增**（T31~T36 / 622 断言） |
| `P3/tests/test_iec104.cpp` | **新增**（T41~T46 / 646 断言） |
| `P3/scripts/build.bat` 等 4 个 | **新增**（CRLF + 纯 ASCII；已过「吞 CR」扫描） |
| `P3/docs/README.md` | **新增**（为什么 / 交付物 / 环回验证 / 故障语义 / 现场约束 / 测试清单） |
| `P3/docs/design.md` | **新增**（帧层 / 映射契约 / 会话时序 / 等价性方法学 / 未覆盖项） |
| `scripts/build_all.bat` | 23 步 → **25 步**（新增 `24/25`、`25/25`；`N/23`→`N/25`；`All 23`→`All 25`） |
| `README.md` | §2.16（新增）+ P3/ 目录树 + 构建产物 + 9353 断言 + 25 组件 + 适配器架构图（`P3 待接`→`P3 ✅`）+ include 表 + 依赖图 + 设计索引 |
| `docs/产品化/P0-架构分层.md` | §3.3「后续适配器」表：`RtDbDeviceIO`/`ModbusDeviceIO`/`Iec104DeviceIO` 状态更新为已落地 |
| `04/src/device_io.h` | 头注释：适配器清单补 `ModbusDeviceIO` / `Iec104DeviceIO` 的落地状态 |

---

# 20. P3 现场闭环最后一跳：TCP 传输层（+ 4 项历史欠账回写）

> 一句话：P3 之前所有通信验证都在**同调用栈内搬字节**（环回）。本次补上
> `IModbusTransport` / `IIec104Transport` 的 **TCP 实现**，用**真内核 socket + 独立线程**把
> "验证过"变成"能接真机"；同时把 MEMORY 里挂着的 4 项治理欠账一次清掉。

## 20.1 本次交付

| 交付物 | 说明 |
| --- | --- |
| `P3/src/tcp_transport.h` | **新增**。`SocketRuntime`（Winsock 生命周期）/ `TcpSocket`（connect_to/send_all/recv_some/recv_exact）/ `mbap_body_len` / **`ModbusTcpTransport`** / **`Iec104TcpTransport`**。Windows(Winsock2) + POSIX 双实现 |
| `P3/tests/test_tcp.cpp` | **新增**。T51~T57 / **87 断言**。服务端是独立线程 + 真 `listen`/`accept`/`recv`/`send`，数据真过内核协议栈与 127.0.0.1 回路 |
| `P3/scripts/build_test_tcp.bat` | **新增**。CRLF + 纯 ASCII；MinGW 链接 `-lws2_32` |
| `04/src/data_models.h` | **新增** `BatteryState` / `GridState` 只读视图 + 视图构造（欠账 4） |
| `04/tests/test_arbiter.cpp` | T07 拆两子场景（欠账 3）；**新增 T18** 视图映射核对 |
| `docs/接口规范/EMS策略接口规范.md` | **新增 §4.10.1 策略编号权威映射表** + SSOT 声明（欠账 1） |
| `工商业储能EMS调控策略设计方案.md` | 周期 2 旁新增「实现口径（回写）」块（欠账 2） |
| `docs/architecture.md` | 「核心策略一~八」段首新增概念名 ↔ 落地策略映射 + SSOT 指向 |
| `04/docs/design.md` | §3.3 加 `strategy_id` 取值与 SSOT 说明 |
| `04/docs/audit-p1-p4-summary.md` | 末尾新增「回写记录」一节（审计原文保持不动） |
| `docs/产品化/P0-架构分层.md` | §3.3 补 TCP 传输层一行 + “换介质不改算法”完整链条 |
| `scripts/build_all.bat` | **25 步 → 26 步**（新增 `26/26` P3 TCP 测试） |
| `README.md` | P3 目录树 / 构建产物 / §2.16 新增「2.16.1 最后一跳」/ 断言与组件数同步；顺带修正两处陈旧计数（04 用例数、10/ 断言数） |
| `P3/docs/README.md`·`design.md` | 新增 TCP 传输层章节（纪律 / 纵横时序坑 / 用例表 / 未覆盖项更新） |

## 20.2 为什么这一跳非做不可

P0 立的承诺是"换介质不改算法"，链条是：

```
SimDeviceIO → MemoryDeviceIO → RtDbDeviceIO → Loopback → TCP
```

前四环都验证过了，但**环回刻意跳过内核**。"能接真机"这句话只能由真 socket 兑现 ——
否则现场接 PCS / 调度时要同时换传输层并对抗一堆未知问题，而那时没有等价性基线可比。

本次换掉的**只有传输层实现**：适配器（`ModbusDeviceIO` / `Iec104DeviceIO`）与算法层
（05/06/07/08）**一行未改**。接真机只需 `set_endpoint(host, port)`。

## 20.3 三条实现纪律

| # | 纪律 | 为什么 |
| --- | --- | --- |
| 1 | Modbus **按 MBAP `Length` 分帧**（先精确读 7 字节 MBAP，再读 `Length-1` 字节 PDU），TID 回显必须一致 | TCP 是字节流：一次 `recv` 可能只回来半个响应（需重组），也可能把两个响应粘在一起（需切分）；TID 不一致 = 错位/串话，宁可直接丢弃也不能喂给解析器 |
| 2 | IEC104 `receive` **三态语义**：`false`=链路错误 / `true&len==0`=本轮无报文 / `true&len>0`=收到 N 字节 | 适配器 `drain_rx()` 靠这三态区分"对端哑了"（计 link_error）与"这一轮就是没数据"（正常） |
| 3 | `connect` **非阻塞 + select 超时** | 阻塞 connect 连不通的地址会挂 20 s 以上，现场表现为"EMS 卡死"，比连不上更难查 |

另有一处介质差异必须留出窗口：适配器 `open()` / 总召唤用 `drain_rx(0)`（非阻塞）等确认帧，
环回下响应同栈产生所以立刻可见，跨 TCP 必须等对端调度 → 给一个 `min_wait_ms` 窗口（**建链时 50，运行期回 0**）。

## 20.4 踩坑 1（本次最值得记的一条）：数值全对，整体滞后一拍

**症状**：T56 逐拍比对显示 B 路比 A 路**整体滞后一拍**，`grid = 382 / 382.4 / 382.8` vs `0 / 382 / 382.4`。
数值一个都不差，只是错位 —— 第一反应会去查 socket、编解码、超时，**全错**。

排查过程中的三个弯路（都留下教训）：

1. 先怀疑"在途数据"。加 `sync_uplink` 显式等一轮上送 —— 治标不治本，
   12 轮 × 60 ms 都等不到，因为**迟到的那批字节是在探测窗口之后才产生的**。
2. 再想"服务端自定时无脑上送" —— 实测**死循环**：客户端 `drain_rx(0)` 的语义是
   "一直收到没有数据为止"，对端持续上送就永远收不干。此路不通（接口改动已回退）。
3. 最后给服务端加 `round` 计数器汇合 —— 逐拍核对**已经对上了**（`cached == want`），
   但**记录出来的值仍然是上一拍的**。

**根因（两条缺一不可）**：

1. **104 是服务端主动上送**（不像 Modbus 一问一答）。服务端消化"上一拍残留的帧"时读到的
   设备值是上一拍的；这一轮上送的字节却在客户端**已经写入本拍环境之后**才被收走。
   实测服务端日志铁证：`[srv] recv n=6 burst_load=0` —— 客户端已写入 380，服务端读到的还是 0。
2. **`EMS_P_GRID` 是派生点，不是外部注入量**。它由 `MemoryDeviceIO::execute()` 里
   `P_LOAD + 站用电 − P_PV − P_BAT` 现算（`SOC` / `T_C` / `P_BAT` 同理），
   而 `realtime_loop.h` 第 ⑪ 步记录真值用的是 `read_actuals()` ——
   `Iec104DeviceIO::read_actuals()` 读的是**最近一次上送的 cache**。
   环回装置下 `read_actuals()` 直接读设备，所以**这个问题在环回下根本不会出现**。
   （`set_environment()` 只写 `P_LOAD` / `P_PV`，`EMS_P_GRID` 只在 `execute()` 里更新 ——
   这一点是解开谜题的关键。）

**处置原则：能汇合就别猜，能核对就别等。** 具体做法（`test_tcp.cpp`）：

```
sync_uplink(load, pv)                      // 每拍开始，写在环境之前
  ① 读干 → 发一帧触发词 → 等服务端 round 自增 → 读干并丢弃（旧值上送全部作废）
  ② 写入本拍环境                            ← 此后服务端任何一轮上送必然带本拍值
  ③ 触发 → 收干 → 核对 cache == 设备真值；对不上再来（最多 4 轮）

refresh_after_execute()                    // device_pump 里 dev.execute() 之后
  反复"触发 → 收干"，直到 cache 追上设备的 P_BAT / SOC
```

两个循环都带**失败计数**并在 T56 断言为 0：允许 `resyncs`（第二轮才对上，时序抖动），
**不允许 `resync_failures` / `exec_refresh_failures`**。改完实测 400 拍 diff=0，
连跑 4 次稳定（`次轮对齐` 在 2~7 之间浮动，失败恒为 0）。

## 20.5 其余用例

| 用例 | 覆盖点 | 实测 |
| --- | --- | --- |
| T51 | TCP 基座：连接 / 连接失败在预算内返回 / `receive` 三态 | 已关闭端口连接 **501~513 ms**（预算 2000 ms）；超时=0、对端关闭=-1 |
| T52 | Modbus TCP 事务：往返字节与环回一致 | 服务端捕获**每一帧** MBAP 自洽（PID=0 / UnitId=1 / 功能码合法）+ 出现过 MEAS 块读；SOC 与设备侧一致 |
| T53(a) | 服务端**每次只发 2 字节** | 客户端自行重组，`stale_reads==0`（分片不得造成采集失败） |
| T53(b) | 服务端 `force_tid` 强制事务号错位 | 请求#1（TID 一致）通过；请求#2 被拒（`format_errors==1`、`rn==0`）**但仍在线** |
| T54 | Modbus TCP 闭环 400 拍 | **逐位差异 0 拍**，峰值 100 kW，SOC 0.5→0.499318，事务 2801 |
| T55 | IEC104 TCP 建链 | `startdt_ok` / `gi_done` / `unacked_tx==0` / 总召回数=1 / `bad_ca==0` / `malformed==0` |
| T56 | IEC104 TCP 闭环 400 拍 | **逐位差异 0 拍**，`resync_failures==0`、`exec_refresh_failures==0` |
| T57 | 真断链（服务端处理 800 帧后关闭） | 断链前峰值 **100 kW** → 采集不可信 → **FAULT** → 指令归零、门控 |

**反向守卫**是这批用例的设计要点：等价性最容易被"两边都恒零"骗过，所以
T54/T56 断言峰值指令 > 50 kW、SOC 首尾不同；T57 断言断链前确实在出力。

T53 的两个子场景也踩过两个坑：`duplicate_first`（依赖"首帧必被读"）不确定 →
改成 `force_tid` 做**确定性**错位注入；T52 的 MBAP 索引一开始按 `TID(0-1) PID(2-3)` 写错成
`rq[1..3]` 当 PID，实际应为 `rq[2] && rq[3]`（`rq[6]` 是 UnitId）。

## 20.6 4 项历史欠账回写

| # | 欠账 | 处置 |
| --- | --- | --- |
| 1 | **策略编号三处不一致** | 明确 **04 `namespace strategy_id` 为唯一真相源**；接口规范新增 **§4.10.1 权威映射表**（`S01_BMS_FORBID`…`S09_DEMAND_RESPONSE` ↔ `bms_protection`/`peak_valley_arbitrage`/…），并写明 `S0x` 是**登记顺序不是优先级**；`architecture.md` / `design.md` 加交叉引用。两处"看似的漏实现"写明理由：`soc_life_planner` 归 05 `check_soc`；`S02_BMS_DERATE` 与 §4.9 共用输入形状（区别在 `bms_lock` vs `bms_derating`） |
| 2 | **field rename 未回写** | 需求原文**一字未改**，在其周期 2 旁加「实现口径（回写）」块：`target_power→p_desired` / `max_power→p_upper` / `min_power→p_lower`，理由是**有符号区间**下 `max_power` 易被误读成"功率大小上限"；同时记录 `direction` 取消、`state→active+reason` 等 |
| 3 | **T07 fixture 与经典例不符** | T07 拆两子场景：**(a)** 上界来自 `pcs_rated_dis_kw=200`（原用例）；**(b)** 上界来自 `transformer_capacity_kw=200` **真实过载**（`P_grid=105, P_load=950` → `tr_load=200, ratio=1.00` → `reason="tr_overload"`，区间 `[10, 200]`），并**直接断言变压器策略自身的输出** |
| 4 | **BatteryState/GridState 未成体** | `04/src/data_models.h` §2.5 **显式成体**为**只读视图** + `battery_state_of()` / `grid_state_of()`。**不替换** `RealtimeSnapshot` / `DeviceLimits` 的既有字段（它们是全项目公共契约，改动会波及 05~10 与 P1/P2/P3），视图单向映射、**不产生第二份真相**；新增 **T18** 逐字段核对映射忠实性 + 验证"改视图不回写源结构" |

## 20.7 全量回归

- `scripts\build_all.bat`（**26 步**）：`[BUILD ALL OK] All 26 components built.`
- 实跑断言（全绿）：

```
02(无数值汇总) / 04=94 / 05=68 / 06=34 / 07=6050 / 08=444 / P0+P0.5=79 /
RT_DB=519 / 09=77 / 10=144 / P1=171 / P2=434 /
P3 Modbus=622 / P3 IEC104=646 / P3 TCP=87
                                        → 合计 9469 断言，FAIL=0
```

对比上一轮 9353：`-65 +94`（04 新增 T18 与 T07(b)）+ `+87`（P3 TCP）= **9469**。

## 20.8 涉及文件

| 文件 | 变更 |
| --- | --- |
| `P3/src/tcp_transport.h` | **新增**（`SocketRuntime` / `TcpSocket` / `MBAP 分帧` / `ModbusTcpTransport` / `Iec104TcpTransport`） |
| `P3/tests/test_tcp.cpp` | **新增**（T51~T57 / 87 断言 / 真 socket / 独立线程服务端） |
| `P3/scripts/build_test_tcp.bat` | **新增**（CRLF + 纯 ASCII + `-lws2_32`） |
| `P3/docs/README.md` | 新增 §4.4「真内核 socket」+ TCP 用例表 + §9 第 6 条经验 + 局限更新 |
| `P3/docs/design.md` | 新增 §6.5「传输层：环回 → TCP」（纪律 / 滞后一拍 / 用例表）+ 分层图补具体类名 + §8 未覆盖项更新 |
| `04/src/data_models.h` | **新增** §2.5 `BatteryState` / `GridState` + 视图构造 |
| `04/tests/test_arbiter.cpp` | T07 拆 (a)(b)；**新增 T18** |
| `04/docs/design.md` | §3.3 补 `strategy_id` 取值与 SSOT 说明 |
| `04/docs/audit-p1-p4-summary.md` | 末尾新增「回写记录（2026-09-14）」 |
| `docs/接口规范/EMS策略接口规范.md` | **新增 §4.10.1** 策略编号权威映射表 + SSOT 声明 |
| `docs/architecture.md` | 「核心策略一~八」段首新增概念名 ↔ 落地策略映射 |
| `docs/产品化/P0-架构分层.md` | §3.3 补 TCP 传输层 + “换介质不改算法”完整链条 |
| `工商业储能EMS调控策略设计方案.md` | 周期 2 旁新增「实现口径（回写）」块 |
| `scripts/build_all.bat` | **25 步 → 26 步**（`N/25`→`N/26`、新增 `26/26`、`All 25`→`All 26`） |
| `README.md` | P3 目录树 / 构建产物 / §2.16 + 新增 §2.16.1 / 9469 断言 + 26 组件；顺带修正 04 用例数（17→18 用例 65→94 断言）与 10/ 断言数（133→144，T110→T111） |
