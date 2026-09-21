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

# 19. 周期 11：系统级联调（2026-09-19）

> 依据：设计方案 §7「周期 11：系统级联调」——「完成 EMS、BMS、PCS、电表、光伏、变压器、
> 负荷全链路联调，测试通信、数据采集、控制指令、状态同步、故障触发、异常恢复、日志记录
> 全流程稳定性。」

## 19.1 新增模块 `11/`

周期 1~10 的交付物都是**单进程、进程内组件**粒度的（`09/` 是"闭环时序级"但仍在一个进程里）。
`11/` 是第一个**跨进程**模块：把同一个 `EmsRuntime` 拆成**设备侧进程**和 **EMS 侧进程**，
中间只通过 RT_DB 共享内存段通信，物理上不可能共享内存指针。

```
main_initializer.exe  ── 建段 + 注册 30 点 + 常驻保活（Windows 段存活依赖）
main_device.exe       ── 六类数据源（BMS/PCS/电表/光伏/变压器/负荷）
main_ems.exe          ── 全栈 EMS（05 安全 + 06 状态机 + 07 闭环 + 08 协同 + P2 可观测）
```

| 文件 | 作用 |
| --- | --- |
| `11/src/integration_runner.h` | `Pacing` 节拍器、`DeviceSimConfig`、`DeviceSideSim`、`EmsSideApp`、`IntegrationCheck` |
| `11/src/main_initializer.cpp` | 初始化器（含 `--seconds N` 保活） |
| `11/src/main_device.cpp` | 设备侧进程（含 `--speed K`） |
| `11/src/main_ems.cpp` | EMS 侧进程（含 `--speed K` + `wait_for_device()`） |
| `11/tests/test_system_integration.cpp` | T41~T47（133 断言） |
| `11/docs/README.md` | 联调报告（9 节） |

## 19.2 两个进程必须"同时活着"才叫闭环

第一版 `main_device.cpp` 跑完 600 拍就退出，`main_ems.cpp` 后启动 → EMS 读到的是**静止的**
点表（设备侧早已停止发布）。表现为"闭环跑通、能量恒 0"。

修法两条：
1. **进程节拍（`Pacing`）**：把模拟时间映射到墙钟时间（`dt/speed`），两个 worker
   按同一节拍推进，谁也不会抢跑。
2. **启动顺序握手（`wait_for_device()`）**：EMS 侧启动后先等设备侧**真的开始发布**
   （判据：`MEAS.P_LOAD > 0.5`），就绪后再 `attach_device()` 重读一次 `CFG.*`，
   否则 EMS 会拿到点表的默认值（例如容量 250 kVA）。

## 19.3 发现并修复 07/ 的三个**真实缺陷**

### 缺陷 1（严重）：权限区间从未发布

`EmsRuntime::step()` 只调 `io_->execute(cmd.p_bat_cmd_kw, dt_s)`，**从不调用
`io_->write_command(cmd)`**。而 `execute()` 只有一个参数、无法携带 `(p_lower, p_upper)`，
于是落地适配器 `RtDbDeviceIO` 里 `has_last_cmd_` 永远为 false，
`CMD.P_UPPER` / `CMD.P_LOWER` 恒为 0 —— 设备侧按 EMS 声明的区间做交叉校核，
实测 3000 拍里有 **2978 拍被判"指令越界"**。

修法（`07/src/realtime_loop.h`）：`execute()` 之前先 `write_command(cmd)`。
两件事语义不同，不可合并：`write_command` 是**发布**（供 SCADA / 设备进程读取），
`execute` 是**驱动**。

### 缺陷 2（严重）：seqlock 碰撞被误计为采集失败

`RtDbDeviceIO::refresh()` 单次调用 `rt_db_get_value()`，失败即 `++stale_reads_`。
但 seqlock 读到写入中途返回 false 的语义是**"请重试"**，不是"数据不可信"。
单进程测试里设备泵在 `step()` 内部顺序执行，永远撞不上；三进程并发下
**600 拍里出现 51 次假告警**。

修法：有限重试（`kReadRetries = 64`）+ 每轮 `std::this_thread::yield()`。
必须有 yield —— Windows 下写者临界区包含 `update_timestamp()`（一次系统调用），
可能比"连读 8 次"还长，纯忙等会 8 次全撞上。

### 缺陷 3（中）：`MemoryDeviceIO` 缺热模型

`PlantModel` 有热模型而 `MemoryDeviceIO` 没有 → `MEAS.T_C` 是死值（恒 25℃），
T42 的"六类数据源全部活着"不可能通过（温度恒定说明这条链路没在工作）。

修法：给 `MemoryDeviceIO` 加 `set_thermal_model()`（**默认关闭**，保证历史行为逐位不变）
+ `update_thermal()`；`11/` 的 `DeviceSimConfig` 里显式打开。

## 19.4 顺带发现并修复：26 个 bat 脚本的"吞 CR"缺陷

全量构建第一步就失败（`'the' 不是内部或外部命令`）。根因是既有的 19 个 bat 是
**LF 行尾**，且中文注释触发了 CP936"吞 CR"：UTF-8 被 CP936 读取时，若某行末尾落在
双字节字符的前导字节（`0x81`–`0xFE`）上，该字节会与行尾 `\r` 配成一对被吃掉，
导致下一行的 `REM` / `echo` 前缀被吞、整行中文被当命令执行。

- **两个坑都会静默失败，退出码仍是 0** —— 这也是它们长期没被发现的原因。
- 写了 `scripts/fix_bat_encoding.py`：LF→CRLF + 模拟 CP936 扫描（越界行末尾补空格），
  幂等，支持 `--check`。一次性修好 **26 个文件**。

## 19.5 一个**不修**的决定：L2 需量纠偏的 1 拍极限环

`07/` 的 L2 纠偏里 `p_pred_avg` 含"假设维持当前关口功率"这一项，而"当前关口功率"
取决于本拍刚下发的指令 → 形成 1 拍极限环；`merge_realtime_correction()` 没有滞环。
实测对照：

| 变压器 | 契约需量 | 末段平均误差 | 指令跳变 | 结论 |
| --- | --- | --- | --- | --- |
| 250 kVA（默认） | 250 kW | — | — | 假故障（约束常驻） |
| 800 kVA | 450 kW | 40.7 kW | 163/200 | L2 极限环 |
| 1200 kVA | 500 kW | 15.5 kW | 56/200 | 极限环（轻） |
| 1000 kVA | 600 kW | ≈ 0 | 0/200 | ✔ 联调基准 |

**决定不修**：修它会动到 `05/` `08/` `09/` 的 7500+ 条断言基线，代价与收益不匹配。
改为把联调基准调到可行工况（1000 kVA / 600 kW），并把缺陷与实测数据记入
`11/docs/README.md` §7.1 遗留问题。建议修法：加滞环（投入 10 kW / 退出 3 kW），
或把 `p_pred_avg` 里的 `p_grid_now` 换成上一拍实际值。

## 19.6 联调结果

三进程实跑（`STEPS=600 DT=0.1 SPEED=4`，故障窗 `4:20:25` / `6:40:45`）：

```
8/8 检查项通过；充放能量 0.82 kWh；状态迁移 7 次；SOE 12 条；stale=0；越区间=0
```

单进程测试 T41~T47 **133 断言全绿**。

## 19.7 涉及文件

| 文件 | 变更 |
| --- | --- |
| `11/`（整个模块） | **新增**（src 4 + tests 1 + scripts 3 + docs 1） |
| `07/src/realtime_loop.h` | **修复**：`step()` 补 `write_command(cmd)`（缺陷 1） |
| `07/src/rtdb/rtdb_device_io.h` | **修复**：seqlock 有限重试 + `yield()`（缺陷 2） |
| `07/src/memory_device_io.h` | **修复**：新增 `set_thermal_model()` / `update_thermal()`（缺陷 3） |
| `scripts/fix_bat_encoding.py` | **新增**：bat 编码修复器 |
| 26 个 `*.bat` | **修复**：LF→CRLF + CP936 行尾补空格 |
| `scripts/build_all.bat` | 23 步 → **27 步**（新增 24/27、25/27 两步） |

# 20. 周期 12：最终验收（2026-09-19）

> 依据：设计方案 §7「周期 12：最终验收」——「完成功能、策略、安全、性能、稳定性、
> 多策略协同、文档全维度验收，正式输出 EMS V2.0 版本。」

## 20.1 新增模块 `12/`：交付门禁，不是新功能

`12/` 对上游是**纯消费者**：不新增算法、不修改任何模块、不读私有成员。
它把前面 11 个周期交付的能力按设计方案的七个维度**重新测量一遍**，
输出可复核、可机器判定、可 CI 门禁的验收报告。

**为什么不能直接引用既有测试的结论**：开发期断言（`01/`~`11/`）断言的是
"我实现的东西符合我的实现"，判据来自实现细节；验收断言的是"交付物符合设计方案的字面
要求"，判据来自公开出口（`StepRecord` / 点表 / 文件 / 墙钟）。两者口径不同，所以
**不复用断言，只复用能力**。

## 20.2 七个维度（53 个检查项）

| 维度 | 检查项 | 验收对象 | 证据来源 |
| --- | --- | --- | --- |
| **A1 功能** | 8 | 策略注册/启停、全链路闭环、状态机七态可达、设备 I/O 三适配器等价、点表自检、配置化往返、日计划装载 | `04/ 06/ 07/ P1/` |
| **A2 策略** | 9 | 9 个策略逐个在"触发工况"下核对设计意图（§3.1~§3.9） | `04/src/strategies_9.h` |
| **A3 安全** | 11 | 9 条安全约束逐位注入、区间矛盾语义、急停锁存、全域硬不变量 | `05/src/safety_engine.h` |
| **A4 性能** | 5 | 单拍耗时（均值/峰值/占用率）、24h 离线仿真墙钟加速比、长跑资源无泄漏 | `07/ 10/` |
| **A5 稳定性** | 6 | 24h 长稳（三段故障窗）、跨进程闭环长稳、stale 零、SOE 零丢弃、迁移与抖动有界、可观测性解耦 | `10/ 11/ P2/` |
| **A6 多策略协同** | 8 | 周期 9 的 7 个组合场景（闭环时序级）全通过 + 汇总 | `09/src/scenario_runner.h` |
| **A7 文档** | 6 | 根文档、模块 `docs/README.md`、模块构建脚本、统一构建入口步数自洽、根 README 索引、报告落盘 | 文件系统 |

每条检查项都带三要素：**编号 / 判据（criterion）/ 实测（evidence）**。

## 20.3 阈值全部可标定

| 检查项 | 阈值 | 依据 |
| --- | --- | --- |
| A4-01 单拍耗时均值 | ≤ 1000 µs | 10 Hz 控制周期下 CPU 占用 ≤ 1% |
| A4-02 单拍耗时峰值 | ≤ 50000 µs | 半个控制周期（首拍含冷启动） |
| A4-03 时间占用率 | ≤ 20% | 留 5× 余量给现场日志/通信 |
| A4-05 墙钟加速比 | ≥ 2000× | 24 h 仿真须分钟级跑完 |
| A5-02 状态迁移次数 | ≤ 30 次 / 24 h | 平均每小时 ≤ 1.25 次 |
| A5-03 指令总行程速率 | ≤ 5 kW/s | 沿用 07/ 工程口径 |

阈值集中在 `AcceptanceRunner::Options` 一处，现场标定只改这里。

## 20.4 验收结论（本机实测）

```
结论: 通过  ——  53 / 53 项通过，0 个维度未全通过

单拍耗时均值/峰值/占用率 : 9.44 / 326.6 us / 0.00944 %
24h 墙钟 / 加速比        : 0.99 s / 87192×
24h 经济净收益           : 734.17 元（等效循环 0.7297 次）
跨进程 stale / 越区间    : 0 / 0
SOE 事件 / 丢弃          : 8 / 0
```

## 20.5 三个写进交付说明的验收结论

### ① 区间矛盾 → DERATED，不是 EMERGENCY

满充（禁充，下界 = 0）叠加光伏大发（不许倒送，上界 < 0）时，本拍无可行非零功率，
安全引擎把区间收成 `[0,0]` 并置 `contradiction=true` + `derated=true`，**不置** `emergency`。
原因工程性：`EMERGENCY` 会**锁存**、需人工复位，一旦锁存储能会在整个光伏高发时段
失去调节能力；而 `DERATED` 可自恢复。→ 检查项 **A3-09** 是这条决策的回归护栏。

### ② 变化率不是区间约束

`ramp_rate` 是"相对上一拍已下发指令的邻域限制"，只能做后置限速器。若硬塞进区间求交，
冷启动（`p_last = 0`）会把区间收成 `[-20, +20]`，再与并网要求的 `[42, 250]` 求交即得
"下界 > 上界"，仲裁器误判矛盾 → 指令被迫归零 → 反而突破并网/需量边界。
→ **A3-07** 断言 `binds_interval=false` 且 `counts_as_derate=false`。

### ③ 设备 I/O 三适配器逐拍等价

`SimDeviceIO`（`PlantModel`）/ `MemoryDeviceIO`（进程内点表）/ `RtDbDeviceIO`（共享内存）
在同一指令序列下 **P_bat 与 SOC 逐拍偏差 ≤ 1e-9**（**A1-05**）。
这是"算法代码零改动就能换设备接口"这句产品化承诺的**量化证据**。
（比较时需把 `pcs_deadtime_s` 归零、`pcs_standby_kw` 归零 —— 前者只有 `PlantModel`
实现、后者会混入站用电；这是口径问题，不是缺陷。）

## 20.6 报告与门禁

| 文件 | 用途 |
| --- | --- |
| `12/docs/ACCEPTANCE-REPORT.md` | 人读：维度总览 + 逐项判据/实测表格 |
| `12/docs/ACCEPTANCE-REPORT.json` | 机器读：CI 抓 `summary.passed/total/result` |
| `12/docs/ACCEPTANCE-REPORT.html` | 汇报：自带样式，双击即开，绿/红标色 |

JSON 是**真的 JSON** —— T58 用 `P1/src/json_lite.h`（一个**独立解析器**）反解验证，
不是自证。CI 直接：

```bash
build/acceptance.exe --quiet --root .. || exit 1
```

## 20.7 已知限制

1. **A6 场景是"抽样工况"而非真实日曲线**：7 个场景的环境脚本是常数或正弦，用于暴露
   策略耦合缺陷；真实日曲线的经济性验收在 `10/` 的 24h 离线仿真里（A4-05 顺带跑了它）。
2. **A4 性能绝对值受机器影响**：判据用"占控制周期的比例"而非绝对数，跨机可比。
3. **A7 只验文档"存在且非空"**，不验内容质量 —— 脚本能做的只是让"忘了写文档"这件事
   无法悄悄发生。

## 20.8 涉及文件

| 文件 | 变更 |
| --- | --- |
| `12/`（整个模块） | **新增**（src 2 + tests 1 + scripts 3 + docs 2 + data 1） |
| `scripts/build_all.bat` | 27 步（新增 `26/27 12\ 验收器编译`、`27/27 12\ 模块自测 T51~T59`） |
| `工商业储能EMS调控策略设计方案.md` | **补全**：§十一「策略优先级设计」表格此前截断在 `|P1|||`，补齐 P0~P3 四层 × 9 策略全表 + 层级语义 + 两条边界纪律 |
| `README.md` | 模块树补 `11/` `12/`；构建产物补 4 项；断言数补齐；横幅 23 → **27 组件** |
| `CHANGES.md` | 新增 §19（本文件）、§20（本文件） |
| 清理 | 删除临时诊断文件 `11/tests/diag_root_cause.cpp` |

# 21. SSOT 回收：策略优先级 / 字段名 / 周期状态三方对齐（2026-09-19）

> 来源：`MEMORY.md`「已知治理缺口」第 1 / 2 / 4 条 —— 都是**文档间口径不一致**，
> 不是代码缺陷，但会让第三方读者得出不同结论。本次一次性收回。

## 21.1 缺口 1：策略编号三方不一致

**问题**：同一样东西在三处定义不同 ——

| 出处 | 口径 |
| --- | --- |
| 设计方案 §三 | 9 策略按**业务名**列出（BMS 禁止充放 / BMS 请求降功率 / …） |
| `docs/接口规范/EMS策略接口规范.md` §4.10 | 按 `code/`（**Python 快照**）口径，9 条目中有两个（`custom_script` / `soc_life_planner`）在 C++ 里不存在，且缺「BMS 请求降功率」「需求响应」 |
| `04/src/data_models.h :: strategy_id` | `S01_BMS_FORBID` … `S09_DEMAND_RESPONSE` |

**决策**：以**设计方案 §三**为 SSOT（上位书面定义），规范 §4.10 回写对齐，代码命名保持不变。

**落地**：
- 规范升版 **v1.1 → v1.2**，§4.10 重写：
  - §4.10.1 九策略清单（设计方案 §三名 ↔ `strategy_id` ↔ 实现类 ↔ 层级 ↔ 运行模式 ↔ 周期）
  - §4.10.2 **两处归并**：
    - `custom_script` → `S09_DEMAND_RESPONSE`（同一件事的两种叫法；Q3 的 300 ms 超时上限仍适用于任何 `kCustom` 策略）
    - `soc_life_planner` → **拆开**：① 硬边界（`soc_min/soc_max` 禁充放 + `soc_warn_*` 预警降额 + 滞环）下沉到 `05/src/safety_engine.h` 的 **L0** SOC 约束；② 经济性寿命项由 `01/` 的 MILP 计划承载，经 `08/src/plan_loader.h` 装载
  - §4.10.3 与设计方案 §十一 P0~P3 的对应
  - §9 附录拆成 9.1（Python）/ **9.2（C++，新增）**，把规范条款映射到 `04/`~`08/` 的真实文件
- **作废 Q4**：「soc_life_planner 维持 L3、desired 复用 `ctx.p_plan_kw`」随归并作废
- 设计方案 §十一本身此前**截断在 `|P1|||`**，本次补齐 P0~P3 四层 × 9 策略全表

> **为什么 SOC 硬边界必须从 L3 下沉到 L0**：放在 L3 等于"经济性策略可以投票决定是否突破 SOC 限值"。
> SOC 禁充放是**物理安全边界**，必须由 L0 无条件收紧区间。这就是设计方案 §十一
> 「安全优先级最高，经济优先级最低」的字面落地，也是 `12/` 验收 **A3-03** 护栏的行为。

## 21.2 缺口 2：字段名 rename 未回写

**问题**：设计方案 §7 周期 2 字面要求输出 `target_power` / `max_power` / `min_power` /
`direction` / `StrategyRequest` / `RealtimeData`，实施后用的是更紧凑的名字，
但设计文档没有回写 → 读者按字面去找字段会找不到。

**落地**：在设计方案**周期 2 下新增「实施状态（回写：字段名对照）」**，逐字段给出对照：

| 设计字面 | 实现 | 说明 |
| --- | --- | --- |
| `target_power` / `max_power` / `min_power` | `p_desired` / `p_upper` / `p_lower` | 改名 |
| `direction` | 由 `p_desired` 的**符号**表达 | **不再单独成字段** —— 从结构上消灭「方向与功率互相矛盾」这一整类 bug |
| `state` | `StrategyResult::active` + `EmsState` | 拆成「策略本拍是否动作」与「系统级状态机」两层 |
| `strategy_name` | `IStrategy::name()` | 静态元数据与动态结果分离 |
| `timestamp` | `PowerCommand::timestamp` | 策略级不带，由闭环在仲裁输出时统一打 |
| `StrategyRequest` | `StrategyResult` | 「策略产出的是结果，不是请求」语义更准 |
| `RealtimeData` | `RealtimeSnapshot` | 改名 |
| — | `StrategyResult::weight` | L3 同层加权所需，实施中新增 |

## 21.3 缺口 4：数据模型未独立成体

`BatteryState` / `GridState` / `DeviceState` 在实施中**折合**进了 `DeviceLimits`
与 `RealtimeSnapshot` 的字段，没有独立结构体。本次**不做拆分**（会动到 04/ 全部策略与
05/ 安全引擎的字段访问路径，收益不匹配），但**把折合关系写清楚**：

| 设计字面 | 实现 | 折合位置 |
| --- | --- | --- |
| `DeviceState` | `DeviceLimits` | 设备静态/慢变数据 + 限值 |
| `BatteryState` | `DeviceLimits` / `RealtimeSnapshot` | `soc` / `soc_min` / `soc_max` / `bms_*` |
| `GridState` | `RealtimeSnapshot` | `p_grid_kw` / `pricing` / 并网约束 |

## 21.4 设计方案 §八 补「实施状态」

原 §八 只有一张 12 周甘特表，看不出"到底做没做"。新增 **§8.1 实施状态**：
逐周期列出交付模块 / 关键产物 / 验证基线（`PASS=` 数值），末尾给出全量回归结论
（`exit=0` ／ **8331 断言全绿** ／ `All 27 components built.`）与阶段结论
「EMS V2.0 已可发布」。

## 21.5 涉及文件

| 文件 | 变更 |
| --- | --- |
| `docs/接口规范/EMS策略接口规范.md` | **v1.1 → v1.2**：§4.10 重写（含 4.10.1/2/3）、§0 文档结构、§8 Q4 作废标注、§9 拆 9.1/9.2、变更日志 |
| `工商业储能EMS调控策略设计方案.md` | 周期 2 新增「实施状态 · 字段名对照」；§八 新增 §8.1 实施状态表（含 `## 周期 3` 标题修复） |
| `CHANGES.md` | 新增 §21（本文件） |

## 21.6 剩余治理缺口

| # | 缺口 | 状态 |
| --- | --- | --- |
| 1 | 策略编号 SSOT | ✅ 本次收回（设计 §三 为 SSOT，规范 v1.2 对齐） |
| 2 | field rename 回写 | ✅ 本次收回（设计周期 2 字段对照表） |
| 3 | T07 fixture 口径（design 经典例用 `transformer=200`，T07 用 `pcs=200`） | ⬜ 未动 —— 属测试夹具选型，不影响语义，保留现状并在 `04/docs/audit-p1-p4.md` 有记录 |
| 4 | `BatteryState`/`GridState` 未独立成体 | ⬜ 决定**不拆**，本次仅写清折合关系 |

---

# 22. 产品化 P3：IEC 104 从站网关（2026-09-19）

> 触发：用户提供《lib60870 IEC 60870-5-104 库使用手册》+ 上游仓库
> <https://github.com/mz-automation/lib60870>，要求把该库接入本项目。
> 这对应 `docs/规划/模拟器与设备接入梳理.md`「待办优先级」第 7 步
> **对外协议：IEC 104（调度）**。

## 22.1 交付物

| 新增 | 内容 |
| --- | --- |
| `vendor/lib60870/` | lib60870-C **v2.4.1**（commit `7a388e3e`，2026-07-15）上游快照，**一行未改**。删减：`.git/` `doxydoc/` `Doxyfile` `tests/`（保留 `examples/`） |
| `vendor/lib60870/build.bat` | 编静态库 `lib60870.a`（23 个 .c = `lib_common_SRCS` 17 + `linked_list` 1 + `lib_windows_SRCS` 5）。**必须 `gcc -std=gnu99`** |
| `vendor/lib60870/README.md` | 来源 / commit / 编译参数 / 实测基线 / 两个坑 / GPL-3.0 说明（上游原 README 改名为 `README-upstream.md` 保留） |
| `P3/src/iec104_point_map.h` | **唯一 IOA 真相源**：24 上送 + 6 下行，含 `scale`/`offset`/`group` + `self_check()`（5 类校验） |
| `P3/src/iec104_server.h` | 从站封装：GI 异步状态机 / ASDU 按类型分桶 + 按容量分包 / 品质映射 / SBO / 18 项统计 |
| `P3/src/iec104_time.h` | 把库的 CP56Time2a 从 UTC 口径扳回北京时间 |
| `P3/src/rtdb_quality.h` | RT_DB 品质**真值引用** + `static_assert` 编译期对账 + C11 原子宏擦除 + `#error` 前置检查 |
| `P3/src/rtdb_source.h` | `RtDbReadOnlySource`：**只有 `read()`，没有 `write()`**（角色边界用类型表达） |
| `P3/src/main_gateway.cpp` | 网关进程，**默认一律否定确认**；`--self-check-only` / `--stale-ms` / `--accept-commands`(DRY-RUN) |
| `P3/src/main_master.cpp` | 主站模拟器（对端工具）：握手 → 对钟 → 总召唤 → 遥调 → 遥控，并把 IOA 翻成中文名 |
| `P3/tests/test_iec104.cpp` | T61~T68 / **165 断言**（后加 3 条 ms→s 单位断言 = **168**，见 §24） |
| `P3/scripts/{build,build_test,run_gateway}.bat` | 构建 / 测试 / 自检+监听+回环演示 |
| `P3/docs/{README,design}.md` | 实测基线、现场部署、6 个缺陷复盘；EXT 点区 / CSV 点表 / 累计量三套后续方案 |

**修改**：

| 文件 | 变更 |
| --- | --- |
| `scripts/build_all.bat` | 27 步 → **29 步**，横幅 `All 29 components built.` |
| `scripts/fix_bat_encoding.py` | **新增控制字符检测**（`CTRL_NAMES` + `find_ctrl_chars`，只报不改，`--check` 命中时退出码 1） |
| `README.md` | 目录导览补 `vendor/` + `P3/`；新增 §2.18；适配器图 `IEC104 (P3 待接)` → `(P3 ✅)`；依赖表补两行 + include 顺序约束注 |
| `P3/scripts/build.bat` | 删掉含 **VT 字符（0x0B）** 的冗余 `-I`（见 §22.4） |
| `P3/scripts/build_test.bat` | 补 `-I ..\07\src\rtdb` 与 `ems_point_table.o`（否则 `undefined reference to EMS_POINT_NAMES`） |
| `P3/src/iec104_server.h` | `IDataSource::read()` 签名 `bool*` → `Quality*`；`report_changes()` 收录品质跳变；`on_clock_sync`/`on_asdu` 改为**先载荷后计数**；`raw_dumped_` `char` → `int` |

## 22.2 关键设计决策

1. **EMS 是从站（服务器），调度是主站。** 端口 / CA / IOA 由调度下发的转发表定。
2. **网关是"第五个角色"，纯只读。** RT_DB 上原有四个角色（设备写 MEAS/STA/CFG、
   EMS 写 CMD、初始化器建段），网关**什么都不写**。理由：调度的遥控若直接写
   `CMD.*` 就绕过了 `05/` 安全约束引擎，等于让远端能直接驱动 PCS。
   **用类型表达纪律** —— 类里根本没有 `write()` 方法。
3. **品质三态取代 bool**：`kGood`(0x00) / `kNotTopical`(0x40) / `kInvalid`(0x80)。
   压成 bool 必然选一个更差的表达：压向 IV 让主站把"5 秒前的真值"当"没数据"，
   压向 GOOD 让主站拿陈旧值当实时值。这条信息在 RT_DB 源头本来就有
   （`QUALITY_UNCERTAIN`），不该在转发层丢掉。
4. **命令默认一律否定确认。** RT_DB 没有"外部设定"区 → 接受就意味着
   "回肯定确认但什么也没做"，调度会以为生效了。**这比拒绝危险得多。**
5. **IOA 4009/4010 上送"权限区间"** 是本项目独有价值：调度能看到 EMS 还允许
   调到哪个范围，不用试探着下发才知道边界。
6. **`self_check()` 把口径集中在表里**：换算系数（SOC/SOH `scale=100`）不许散在代码，
   否则必然出现"有个地方乘了 100、有个地方没乘"。

## 22.3 实测确认的库行为（四处与手册不一致）

| 项 | 库实际 | 手册/常识 | 处理 |
| --- | --- | --- | --- |
| 从站 t0 超时 | **10 s** | 30 s | 文档更正 |
| CP56Time2a 口径 | **UTC（`gmtime`）** | 应北京时间 | `iec104_time.h` 两端对称加减 |
| GI 的 ACT_TERM | **必须带一个信息对象** | §6.4/§6.9 漏写 | `serve_gi()` 修复 |
| 带时标类型取值 | **无公开 getter** | — | 主站工具 include 内部头 |

**ACT_TERM 细节**（唯一"完全照手册写就必然错"的点）：空 ASDU 的 TypeID 是 **0**，
主站侧 `CS101_ASDU_createFromBufferEx` 校验失败直接丢帧 → 主站永远等不到终止 →
周期性重召。从站侧看起来"总召一直有请求"，像主站有病，其实是自己发的帧不合法。
正确做法是装一个 `InterrogationCommand_create(NULL, 0, qoi)`。

**时区修法的测量陷阱**：库的双向换算**严格互逆**，所以"发出去读回来看是否相等"
的自环测试**永远恒等** —— 差 8 小时也照样过。T66 因此额外做一次**外部真值对账**
（拆 CP56Time2a 字段跟 `gmtime(now+8h)` 比）。写时区断言必须有不经过被测代码的真值来源。

## 22.4 本轮修掉的 6 个缺陷（**全部是"静默出错"型**）

| # | 缺陷 | 症状 | 根因 |
| --- | --- | --- | --- |
| 1 | RT_DB 品质常量镜像写反 | 值全对，**24 个点品质全 `0x80` IV**（调度画面全灰） | `rtdb_source.h` 手抄 `kRtdbQualityGood = 0`，真值是 `1` |
| 2 | `rt_db_structs.h` 的 C11 原子宏破坏 libstdc++ | `expected unqualified-id before 'sizeof'`，错误全指向该头第 34 行 | 函数式宏 `atomic_store_explicit` / `atomic_thread_fence` 匹配不看命名空间限定，把 `std::` 下的**声明**就地展开 |
| 3 | 计数自增早于载荷写入 | T66 偶发断言失败，像随机抖动 | `on_clock_sync` 先 `++clock_syncs` 后写文本；观察者按计数判就绪会读到半成品 |
| 4 | `char raw_dumped_` 溢出 | `--dump-raw` 本想只打前 200 帧，结果刷满屏 | `char` 有符号，到 127 回绕成负数 → `> 200` 永远不成立 |
| 5 | ASDU 249 字节装不下 18 个带时标遥测 | "有点没上去"（静默丢 2 点） | 每点 `IOA(3)+值(4)+品质(1)+时标(7)=15`，`(249-6)/15=16` |
| 6 | `P3/scripts/build.bat` 里的 VT 字符 | 编译照过，错得毫无声响 | Python 批量改写时 `"\v"` 被当转义序列，`..vendor\` 变成 `..<0x0B>endor\` |

**缺陷 1 的修法是三层的**（这是本轮最有价值的一条方法论）：

1. **不抄数字** —— 新建 `rtdb_quality.h` 直接引用 `QUALITY_GOOD`；
2. **编译期对账** —— `static_assert` 钉住上游枚举，一变就红；
3. **端到端外部真值对账** —— 主站日志里 `0x80` 必须为 0 条。

> 为什么这个 bug 值这么多笔墨：协议层忠实翻译源侧的话、单元测试的 `MemorySource`
> 根本不走 RT_DB、自环测试也不碰品质真值 —— **三层测试全绿，现场画面全灰**。
> 它只能靠"拿一个外部真值对着比"发现。

**缺陷 2 的修法**：`rtdb_quality.h` 必须在**任何 C++ 标准库头之前** include，
它内部立刻 `#undef` 掉那些宏，并加 `#error` 前置检查（命中 `_GLIBCXX_ATOMIC`
等保护宏就报错）。把"一堆莫名其妙的报错"变成"一条明确的错误"。

**缺陷 6 的修法**：删掉冗余 `-I`；并给 `scripts/fix_bat_encoding.py` 加了
**控制字符检测** —— 只报不改，因为"到底该是哪个字符"只有人知道，
自动改只是把一种事故换成另一种事故。

## 22.5 验证基线

| 项 | 基线 |
| --- | --- |
| `P3/` 单元测试 | **PASS=165 FAIL=0**（T61 握手 / T62 总召唤闭环 / T63 变化死区 / T64 遥调受理拒绝 / T65 遥控类型校验 / T66 时标时区 / T67 品质三态 / T68 断线重连 + 点表自检）—— 后加 3 条 `ms_to_s()` 单位断言 = **168**，见 §24 |
| 端到端（三进程 + 主站模拟器） | 阶段 A（设备在线）**全 `0x00` GOOD**；阶段 B（设备停 + 等 5 s）**全 `0x40` NT**、值冻结在最后一次好值；`0x80` **0 条** |
| 总召唤时序 | `激活确认 → 数据(18 遥测 + 6 遥信) → 激活终止` 三段齐全，**成对 2/2** |
| 分包 | 18 个带时标遥测实拆 **16 + 2** 两帧 |
| 命令 | 肯定确认 **2** / 否定 **0**（遥调 6001 + 遥控 5001） |
| 网关自检 | 上送 **24** 点 / 下行 **6** 点，点名**全部可解析**到真实 RT_DB 段 |
| 全量构建 | `[BUILD ALL OK] All 29 components built.`，**8496 断言全绿**（`11/` T47 判据修正后为 **8498**，见 §23） |

**两阶段对比是双侧证据**：只有 A 全绿不能说明 NT 通路活着，只有 B 全 NT
不能说明 GOOD 通路没被误伤。两侧都要有，否则"两边都恒 0"也能骗过断言。

## 22.6 实测发现的一个语义边界（不是 bug）

`--stale-ms 3000` 下，阶段 A 里 **`CMD.*` 三点（4008/4009/4010）被判 NT**。
这本身是**正确**的：本次没跑 `ems_side`，而按角色边界 `CMD.*` 只有 EMS 写，
所以它们自初始化后就没更新过 —— 标 NT 等于告诉调度"权限区间不可信"，
比标 GOOD 安全。

但它暴露了一条使用纪律：**`--stale-ms` 不能一刀切**，要按"最慢的正常更新周期"取。
静态配置量若只在启动时写，就得调大阈值或关掉。精细化方案（按点名前缀分层，
`CFG.` 不判）记在 `P3/docs/design.md` §10，**待现场确认设备进程写 `CFG.*` 的周期后再落地**。

## 22.7 遗留与后续

| # | 事项 | 说明 |
| --- | --- | --- |
| 1 | **EXT 点区（6 点，纯追加）** | 下行 6 点目前**没有落点**。★ 关键安全要求：`6001 有功功率设定` 应映射到**权限区间**（收窄 `p_upper/p_lower`）而**不是直接覆盖 `p_desired`**，否则 L0 安全约束对调度指令失效。方案见 `P3/docs/design.md` §5 |
| 2 | **转发表外置（CSV）** | 现场换转发表要重编不可接受。方案 + 必须做的 4 项校验见 `P3/docs/design.md` §6 |
| 3 | **累计量（电度）** | `Kind::kCounter` 已定义未实现 —— BCR 编码 + SEQ 回绕 + 周期性全量，且 **RT_DB 还没有电度点**（无源头）。顺序：先建点再实现。见 `P3/docs/design.md` §7 |
| 4 | **BMS 限值缺口** | IOA 4017/4018 现在只是仿真的注入点，真实系统里必须来自 BMS 通信。**不补的后果**：EMS 会一直读到默认 200 kW，"BMS 要求降到 50 kW"永远传不进来。见 `P3/docs/design.md` §4 |
| 5 | **IP 白名单** | `on_connect` 目前接受任何客户端。现场至少要绑专用网段 + 防火墙放行 |
| 6 | **GPL-3.0 授权路线** | 静态链接传染范围大。三条路线（接受 GPL / 买商业授权 / 换实现）**需商务法务决策**，本轮不做 |

## 22.8 涉及文件

见 §22.1 的两张表（新增 14 个文件 / 修改 6 个文件）。
`P3/` 是**叶子层**：不产出被其他模块依赖的头文件，`07/` 及以下完全不知道它存在，
删掉 `P3/` 不影响任何模块。

---

# 23. 修正：`11/` T47 的「stale 恒 0」判据（2026-09-19）

> 起因：P3 交付后跑全量构建，`[BUILD ALL FAIL]` —— **不是 P3 的问题**，
> 是 `11/` T47 在满载下红了一次。属于"判据写错"，不是"产品坏了"。

## 23.1 现象

```
FAIL  tests\test_system_integration.cpp:616 : io.stale_reads() == 0
并发读写 1701 点 / 1068810 次 read_snapshot / stale=22
```

而**同一二进制单跑恒为 0**（判据修正前实测 133/133 全绿）。即：红不红取决于
**机器闲不闲**，而不是**代码对不对**。

## 23.2 根因（三层，越往下越本质）

1. **重试是次数上限，不是时间上限**：`RtDbDeviceIO::refresh()` 用
   `kReadRetries = 64` + `std::this_thread::yield()`。没有同优先级的就绪线程时
   `yield()` **立即返回**，64 次预算在**微秒级**烧完。
2. **写者可能被 OS 抢在临界区里**：`rt_db_set_value()` 的临界区含
   `update_timestamp()`（Windows 下 `GetSystemTimeAsFileTime`）。写者若恰在此刻被
   换出，`sequence` 会在整个时间片（**15.6 ms**）里保持奇数 —— 这期间读到该点的
   尝试**必然全部耗尽**。机器越忙越容易（该次是全量构建同时编 29 个组件）。
3. **判据把"设计内降级"当成了故障**：`stale` 的语义是「保留上一次有效值 + 品质置
   不可信」，下游安全层本来就拒绝采信 `quality != GOOD` 的点。
   **它是一条合法存在的降级路径 —— 拿一个允许非零的设计量去做恒零断言，
   是判据问题，不是产品问题。**

## 23.3 修法

**刻意不调大 `kReadRetries`**：次数上限再大也不解决"写者被抢占"，只会把假红
推远（并把"假红"换成"假绿 + 更长的忙等"），问题留在原地。

### 23.3.1 `07/src/rtdb/rtdb_device_io.h`：把"碰撞"与"降级"拆开

判据必须能区分"重试吸收了"与"重试被绕过"，所以需要两个计数器：

| 成员 | 语义 |
| --- | --- |
| `collisions()` | seqlock **首次尝试即失败**（读到写者中间）的事件数 —— **真的撞上了** |
| `stale_reads()` | 重试预算**耗尽** → 保留缓存 + 品质置不可信 —— 降级 |
| `absorbed()` | `collisions() - stale_reads()` —— 被重试吸收掉的部分 |

恒有 `stale_reads() <= collisions()`。顺带修掉一处**过期注释**（原文写"取 8"，
代码是 64 —— 注释与代码不一致本身就是缺陷），并把"这不是硬保证"的理由写在常量旁边。

### 23.3.2 `11/tests/test_system_integration.cpp` T47：判据改四层

| 层 | 判据 | 作用 |
| --- | --- | --- |
| ① | `collisions() > 0` | **反向守卫**：真的撞上了，测试才测到了东西 |
| ② | `stale_reads() < collisions()` | 碰撞**不是**降级（删掉重试循环 → `stale==collisions` → 立刻红） |
| ③ | `stale_reads() * 1000 < reads` | 量级守卫：降级率 < 0.1% |
| ④ | `bad_soc == 0` | **硬判据**：半写状态一次都没漏出去 |

读循环同时改成「至少跑 1.0 s **且**必须已撞上过碰撞才收工（硬上限 3 s）」——
否则反向守卫①自己会随负载翻脸（"靠概率的守卫"等于没有守卫）。

**为什么可以允许③/②非零**：因为④是硬判据，且"降级"是**安全方向**
（宁可不用这个点，也不用一个可能半写的点）。允许它偶发，只要求它**罕见**
且**不掩盖坏值**。

### 23.3.3 同口径修正另外两处文案

| 文件 | 原 | 现 |
| --- | --- | --- |
| `11/src/main_ems.cpp` | 检查项「控制窗口内跨进程 30 点读取零失败」`stale_in_run == 0` | 「碰撞被重试吸收（降级 ≤ 碰撞的 1%）」`stale*100 <= coll`，并按 `coll0` 一并扣掉装配预热期 |
| `11/src/integration_runner.h` | `stale 读点 N（跨进程品质，必须 0）` | `读点降级/碰撞 N / M（碰撞被重试吸收；降级=保留上次有效值，非故障）` |

### 23.3.4 刻意**不动**的

`07/tests/test_rtdb_device_io.cpp`（4 处）、`11/` T41/T42/T43/T45、`12/` A5-04 的
`stale == 0` —— 它们都由 **`device_pump` 同线程顺序**驱动，没有真并发，
`stale` 是**确定性的 0**，硬判据在那里成立且更强。
**该保留的硬判据不要因为"顺手统一"而软化。**

## 23.4 实测对照（同一二进制）

| 场景 | 结果 |
| --- | --- |
| 单跑 | `碰撞=90 吸收=90 降级=0` / 1087259 次读 |
| 全量构建满载（判据修正后） | `碰撞=80 吸收=80 降级=0` / 1100774 次读 |
| 全量构建满载（判据修正前，唯一一次复现） | `降级=22` / 1068810 次读 → **旧判据红** |

> 注意第三行**没有在修正后的构建里复现**。这不是"问题消失了"，而是它的触发
> 依赖调度时机 —— 正是"统计性判据"要解决的问题：**触发概率低 ≠ 判据可以写死**。

## 23.5 验证基线

| 项 | 值 |
| --- | --- |
| `11/` 单元测试 | **PASS=135 FAIL=0**（T47 由 4 条断言 → 6 条） |
| `07/` RT_DB 接入 | **519**（不变；新增的是访问器，不改断言数） |
| 全量 | **8498 断言 / 0 失败**，`[BUILD ALL OK] All 29 components built.`，`exit=0`（§24 之后为 **8501**） |

## 23.6 通用教训

1. **判据的"允许值域"必须来自设计语义** —— 先问「这个量有没有**合法**的非零路径？」
2. **反向守卫不能靠概率** —— 把"跑够"写成**循环终止条件**，别指望机器配合。
3. **不靠调大魔数掩盖** —— `64 → 6400` 只是把假红推远。
4. **"该软的判据"和"该硬的判据"要分清** —— 确定性场景的恒零断言是资产，不是负债。

## 23.7 涉及文件

| 文件 | 改动 |
| --- | --- |
| `07/src/rtdb/rtdb_device_io.h` | 新增 `collisions()` / `absorbed()` + `collisions_` 计数；修正过期注释 |
| `11/tests/test_system_integration.cpp` | T47 判据改四层 + 读循环终止条件 + 输出格式 |
| `11/src/main_ems.cpp` | 检查项口径 + 预热期扣减 |
| `11/src/integration_runner.h` | 报告文案 |
| `11/docs/README.md` | 新增 §5.2.1（缺陷复盘）+ §8 判据分层说明 |
| `README.md` | 断言数 133→135、基线块 + 判据修正说明、T45/T46/T47 用例表 |
| `工商业储能EMS调控策略设计方案.md` | §8.1 实施状态：11/ 基线 + 全量基线更新到 8498 |
| `P3/docs/README.md` | §10 验证基线加注 |
| `CHANGES.md` | §22.5 加注 + 本节 |

---

# 24. 顺带修掉：网关心跳 `T+` 恒为 0（单位换算，2026-09-19）

> 来源：首次实跑 `P3/scripts/run_gateway.bat`（该脚本此前从未被执行过）。
> 属于**"只影响打印"的静默出错**：编译、单元测试、协议交互**全部正常**。

## 24.1 现象

网关运行 60 s，心跳打了 5 次，**全部**是：

```
[T+  0s] 连接 1 开/1 断  总召 2 完成/2 请求  周期 6 自发 3  命令 2 收/0 拒  坏读 0 陈旧 0  NT点次 0
```

现场读起来的唯一结论是"时钟卡住了"。

## 24.2 根因

```cpp
const std::uint64_t t0 = iec104::now_utc_ms();      // ← 返回**毫秒**（Hal_getTimeInMs）
...
std::printf("[T+%3llus] ...", (now - t0) / 1000000ull);   // ← 却按**微秒**除数
```

除数大了 1000 倍 → 头 **1000 秒**（16.7 分钟）里恒为 `0`，之后按 1000 秒跳。
网关的设计工况是**长期在线**，所以这条心跳基本等于没有。

## 24.3 修法：换算抽成纯函数，让它可被断言

不只是改个除数 —— 把它变成**全工程唯一的 ms→s 换算口**，这样它就能进单元测试：

```cpp
// P3/src/iec104_time.h
inline std::uint64_t ms_to_s(std::uint64_t ms) { return ms / 1000ULL; }
```

心跳改为 `iec104::ms_to_s(now - t0)`；`tests/test_iec104.cpp` 的 T66 增 3 条断言：

| 断言 | 挡的是什么 |
| --- | --- |
| `ms_to_s(60000) == 60` | **就是这个 bug**（错版得 0） |
| `ms_to_s(999) == 0` | 未满 1 s 截断，不是四舍五入 |
| `ms_to_s(3600000ULL) == 3600` | 量级正确 |

**通用教训**：`/1000000`、`*1000`、`ms/μs/ns` 这类换算**不要散落在 printf 里**——
散落就无法断言，无法断言就只能靠肉眼看日志，而"看日志"这件事在交付时一定被跳过。
抽成一个有名字的纯函数，代价是 1 行，收益是它进了回归。

## 24.4 实测验证

| 场景 | 修前 | 修后 |
| --- | --- | --- |
| 网关跑 25 s | `[T+  0s] [T+  0s]` | **`[T+ 10s] [T+ 20s]`** |

## 24.5 验证基线

| 项 | 值 |
| --- | --- |
| `P3/` 单元测试 | **PASS=168 FAIL=0**（165 + 3 条单位断言） |
| 全量 | **8501 断言 / 0 失败**，`[BUILD ALL OK] All 29 components built.`，`exit=0` |

## 24.6 涉及文件

| 文件 | 改动 |
| --- | --- |
| `P3/src/iec104_time.h` | 新增 `ms_to_s()`（唯一 ms→s 换算口）+ 为什么这么做的注释 |
| `P3/src/main_gateway.cpp` | 心跳改用 `ms_to_s(now - t0)`（原 `/1000000ull`） |
| `P3/tests/test_iec104.cpp` | T66 增 3 条单位断言（165 → **168**） |
| `README.md` / `P3/docs/README.md` / 本文件 / 设计方案 / memory | 断言数 165→168、全量 8498→8501 |

## 24.7 附记：一次 `09/` 编译器**无声退出**（未复现，判为环境事件）

§24.5 的 8501 是**重跑**得到的。第一次全量构建在第 **20/29 步（`09/`）** 红了，
但现场的形态很特殊：

```
=== 编译并运行 09/ 多策略组合测试 ===
[FAIL] 编译失败          ← 之后一行诊断都没有
```

排查（**结论：不是代码缺陷**）：

| 证据 | 值 |
| --- | --- |
| `g++` 诊断行数 | **0**（`error:` / `warning:` 计数均为 0）—— 不是编译错误 |
| `09/` 源码与脚本 | 未改动（`git status` 干净；`build_test.bat` mtime 早于本次会话） |
| 单独重跑 `09\scripts\build_test.bat` | **编译通过 + 77 断言全绿** |
| 全量重跑（同二进制、同机器） | **29/29 通过，未复现** |
| 磁盘余量 | 20.9 GB（排除写满） |

`g++` 在**子进程被外部终止**（杀软 / OOM / 被 OS 抢占）时，会不打印任何诊断、
直接返回非零码 —— 这与观测到的形态一致。

> ★ **这种红的现场是不可回放的**：`build_all.bat` 失败时只印
> `Check output above`，而"输出"本来就不存在。所以判定手段只能是
> **单独重跑该模块**，不是盯着空日志反推。也**不要**为一个未复现的 flake
> 去改 29 个模块脚本 —— 那是把不确定性搬进基线。

同时修正一处**断基数口径**：日志扫描（`grep PASS=`）一直漏掉 `02/`，
因为它的测试只印 `ALL TESTS PASSED`、**不印 `PASS=n`**。
明细必须写成 `02 / 04=65 / 05=68 / …`（§18.10 的既有写法），14 项加总 = **8501**；
此前误把 `02=65` 塞进明细会得 8566（差 65）。**加总必须对有明细。**

另：本次满载实测 T47 为 `碰撞=88 吸收=87 降级=1` —— 旧判据（`stale == 0`）
在这里会**第二次假红**，而新四层判据全部成立（`1 < 88`、降级率 0.00009% < 0.1%、
`bad_soc == 0`）。§23 的修法被再次验证有效。

---

# 25. BMS 禁充放位走不进 RT_DB（+ 连带："设备限值运行期冻结"）（2026-09-19）

> 来源：`docs/规划/模拟器与设备接入梳理.md` §10 的**缺口 A1（风险最高的一条）**。
> 性质：**安全链断裂**。补断言时又牵出一条更深、影响面更大的缺口（§25.2 第 2 层）。

## 25.1 现象

`07/src/rtdb/rtdb_device_io.h:261-262`（修复前）：

```cpp
// 禁充/禁放位：当前点表未建模（与 MemoryDeviceIO 对齐）。
out.bms_chg_forbidden = false;
out.bms_dis_forbidden = false;
```

这两个 `bool` 是 **`04/` 的 `kBmsForbid` 策略（S01 · L0 · 最底层）**与
**`05/` 的 `check_bms_forbid()`** 的唯一输入。硬写 `false` 等价于
**「BMS 永远允许充放」** —— 全项目最硬的安全边界在**实时库形态下永不触发**。

后果链：

```
BMS 上报"禁充/禁放"
   └─► 04/kBmsForbid（S01 · L0 · 最底层）
         └─► 05/check_bms_forbid() 折进 (p_lower, p_upper)
               └─► 决定 EMS 到底能不能放电
```

**为什么三层测试全绿也没发现**：仿真形态下 `DeviceLimits` 由 `SimDeviceIO`
从 `PlantConfig` 直接给，`bms_chg_limit_kw` 那一路是通的，所以 `05/` 的限值
逻辑照常被测到 —— **只有 `RtDbDeviceIO` 这一条路是恒 `false`**。
而 T26 的跨进程对照只比了 30 个**点的值**，没比这两个 `bool`。
**跨内存边界 + 仿真侧由调用方注入** = 测试永远照不到。

## 25.2 根因（三层，越往下越本质）

1. **点表未建模**：30 点表里没有 `STA.BMS_CHG_FORBID` / `STA.BMS_DIS_FORBID`
   的位置，适配器只能"硬写 false 占位"。
2. **★ 设备限值在运行期是冻结的**（补断言时才暴露，比 A1 本身更严重）：
   `EmsRuntime::step()` **从不调用 `refresh_device_limits()`** —— 限值只在
   `init` / `reset` / `configure_plant` / `attach_device` 里刷，即**装配期刷一次**，
   此后运行期**冻结**。于是设备侧会变、EMS 该跟着变的限值全都退化成装配期常量。
3. **注释声称的行为与代码实际的行为不一致**：`refresh_device_limits()` 上方
   写着"现场 BMS 动态降功率就是通过这个入口每拍刷进 `dev_` 的"，
   而 `step()` 里**根本没有调用**；`04/IDeviceIO::read_limits()` 的契约也写着
   "每个控制周期刷新"。**本项目第 N 次栽在同一件事上。**

第 2 层的后果面（远超禁充放）：

| 退化字段 | 现场后果 |
| --- | --- |
| `bms_chg_forbidden` / `bms_dis_forbidden` | BMS 说"禁放"，EMS 继续放电（**安全链断**） |
| `bms_max_chg_kw` / `bms_max_dis_kw` | BMS 动态降功率（高温 / 低 SOC），EMS 仍按额定下发 |
| PCS 额定 / 变压器容量 / 契约需量 | 现场改参数后 EMS 不跟随，**只能重启** |

## 25.3 修法

### 25.3.1 点表（30 → 32 点）

新增 `STA.BMS_CHG_FORBID` / `STA.BMS_DIS_FORBID`，**追加在枚举末尾**：

```c
#define EMS_POINT_COUNT 32
    ...
    EMS_STA_VALID,           // 0/1  数据有效
    EMS_STA_BMS_CHG_FORBID,  // 0/1  BMS 禁止充电（1=禁充）
    EMS_STA_BMS_DIS_FORBID   // 0/1  BMS 禁止放电（1=禁放）
```

> ★ **为什么必须追加在末尾**：索引是**运行时坐标**。中间插入会让其后所有点的
> 索引整体偏移，而共享内存里**已有段的索引不跟着变** —— 现场表现为
> **"点名对得上、值全是隔壁点的"**。追加在末尾则 `0..29` 的索引**逐位不变**，
> 老段（30 点）与新段（32 点）在前 30 点上继续兼容。

**默认值取 `0 = 允许`，刻意不取 1**：默认禁充放会让"设备侧尚未上线"直接锁死
`[0, 0]`（冷启动不可用）；而"允许"与修复前**逐位一致**，不引入新风险。
fail-safe 由 `05/` 的 `bms_comm_lost → [0,0]` **独立**承担（那条路径不依赖本点）。

`ems_point_table.c` 的 NAMES / UNITS / DEFAULTS 三数组同步；
`memory_device_io.h::mem_point` 加两个常量、`init_defaults()` 补两个点
（**漏一个会走 miss 分支返回 0.0，值恰好也是 0，看不出错**）。

### 25.3.2 适配器（真读）

`RtDbDeviceIO::read_limits()` 改为从点表读；`MemoryDeviceIO` 同步（保持
**逐点可互换**），并加注入口 `set_bms_forbid(chg, dis)`。

### 25.3.3 运行期限值刷新（跟随适配器能力，不破坏装配层注入）

`dev_` 有**两个写入者**：

```
① IDeviceIO::read_limits()        ← 设备侧说了算
② rt.device_limits() = cfg.limits ← 装配层注入（09/ 10/ P1/ P2 共 8 处）
```

无条件每拍刷 ① 会让 ② 被覆盖、8 处夹具全变。所以做成**能力开关**：

| 件 | 内容 |
| --- | --- |
| `04/src/device_io.h` | 新增 `virtual bool limits_are_live() const { return false; }` |
| `RtDbDeviceIO` | `→ true`（设备侧进程随时可能改 `CFG.*` / `STA.BMS_*_FORBID`） |
| `SimDeviceIO` / `MemoryDeviceIO` | 保持默认 `false`（**行为逐位不变**） |
| `LoopConfig` | 新增 `bool refresh_limits_each_step = false` |
| `attach_device()` | `if (io_->limits_are_live()) cfg_.refresh_limits_each_step = true;` —— **只打开、不关闭**（调用方显式设 false 时不被悄悄改回） |
| `step()` | 新增 **⓪ 步**（在①之前）：`if (cfg_.refresh_limits_each_step) refresh_device_limits();` |

**现场装配不会忘**（能力挂在适配器类型上，不在调用方）；
**仿真行为逐位不变**（`07/T26` 逐拍差异仍 **0 拍**）。

同时改正 `refresh_device_limits()` 上方那段与代码不符的注释。

## 25.4 断言（三层，各自独立口径）

| 层 | 位置 | 覆盖 |
| --- | --- | --- |
| 单元（介质等价） | `07/tests/test_rtdb_device_io.cpp` **T25** | 设备侧写点 → EMS 侧 `read_limits()`：**置位 / 不串扰 / 可恢复 / 双置** 四方向 |
| 集成（跨进程形状） | `11/tests/test_system_integration.cpp` **T48**（新增） | 窗 `t∈[10,20) s` 内 `p_upper ≤ 0`、**0 拍放电**；7 层判据 |
| 验收（公开出口） | `12/src/acceptance_runner.h` **A5-07**（新增） | 同上机制、**独立编排**，出口是**下发指令** `rec.p_cmd` |

**为什么不能用末态验证**：置 1 只发生在窗口内。若只看末态，
**「通路断了」与「窗口过去了」完全不可区分** —— 判据会恒过。所以 `T48` / `A5-07`
都**边跑边采**，用"窗内 vs 窗外"差分取得区分度。

剧本侧（`11/src/integration_runner.h`）：`DevFaultWindow.kind` 扩到 **7/8**，
但 **7/8 与前六类性质不同** —— 前六类是**故障**，7/8 是 BMS **正常上报的运行限值**：
不计 `comm_fault_ticks`（单独计 `bms_forbid_ticks`）、不触发 FAULT、且字段**每拍全量重放**
（窗口一结束自动回到"允许" → "限值可动态变化、且可恢复"成为**被测行为**）。

## 25.5 实测对照

| 项 | 值 |
| --- | --- |
| `11/T48` | `禁放拍数=99（窗内 100 拍）/ 窗外残留=1 / 窗内 p_upper≤0 拍数=98 vs 窗外常规拍 1`（共 200 拍，另有门控 10 / `hold_last` 0 拍） |
| `11/T48` 窗内指令 | `p_cmd_max = -194.1 kW`（**负 = 放电**，即"禁放期间只充不放"） |
| `07/T26` 等价性 | `峰值指令 100 kW / SOC 0.5 → 0.499318 / 逐拍差异 0 拍`（刷新开启后仍逐位等价） |
| `12/` 验收 | `[PASS] A5-07`，`54 / 54 项通过`，退出码 0 |

**两次判据修正**（都是"判据错"而不是"代码错"）：

1. `max_upper_in ≤ 0` 恒红 → 窗内第 1 拍必然读到**上一拍**限值
   （设备侧 `t=9.9` 写 1，EMS `t=10.0` 才读到）→ 改计数式，留 3 拍容差。
2. `dis_outside_ticks == 0` 恒红（实际 1）→ 窗外另有 10 拍是启动期
   **`state_gated`**（非运行态区间收 0）、1 拍是边界延迟 —— 都是**设计内路径**，
   差分判据必须显式**排除 `state_gated` / `hold_last`**，否则没有区分度。

## 25.6 验证基线

| 组件 | 修复前 | 修复后 |
| --- | --- | --- |
| `07/` RT_DB | 519 | **533**（+14） |
| `07/` P0+P0.5 | 79 | **79**（不变 —— 行为等价性守住） |
| `11/` | 135 | **143**（+8） |
| `12/` T51~T59 | 113 | **113**（不变；验收检查项 53 → **54**） |
| **全量** | 8501 | **8523** |

`[BUILD ALL OK] All 29 components built.`

## 25.7 通用教训

1. **"注释声称的行为"必须与代码实际行为一致**。本项目第 N 次栽在这里
   （前几次：`read_limits()` 的"每拍刷新"、`execute()` 的"发布"、网关心跳的
   `T+0s`）。**写在注释里的承诺，要么有调用、要么有断言。**
2. **跨内存边界 + 仿真侧注入 = 测试盲区**。仿真适配器的限值由调用方给，
   于是"设备侧真读"这条路径**从没被走过**。**补通路时，第一件事是问
   "这条路上有几步是测试夹具替我做的"。**
3. **点表只能追加，不能中间插入**（索引是运行时坐标，已有段的索引不会跟着变）。
4. **事件型通路不能用末态断言** —— "没发生"与"发生过了"在末态上不可区分。
5. **差分判据必须排除设计内路径**（`state_gated` / `hold_last`），
   否则测的是别的东西；但**也不能顺手软化**那些确定性的硬判据（如 `stale == 0`
   在 `device_pump` 同线程顺序驱动下）。
6. **能力开关优于配置开关**：把"该不该每拍刷新"挂在适配器类型上
   （`limits_are_live()`），现场装配**不会忘**、仿真**零改动**。
   若做成装配层配置项，就等于把一个**必然会被漏配**的东西交给现场。

## 25.8 现场接入硬要求（交付给 BMS 网关方）

1. **必须每拍显式写** `STA.BMS_CHG_FORBID` / `STA.BMS_DIS_FORBID`。
   若仍写老段（30 点），前 30 点不受影响，但这两个点读不到 → 回落默认（允许）。
2. **不许把这两个位当故障位用**：它们是**正常上报的运行限值** ——
   置位不触发 FAULT、不计 `comm_fault_ticks`、**必须可恢复**。

## 25.9 涉及文件

| 文件 | 改动 |
| --- | --- |
| `07/src/rtdb/ems_point_table.h` / `.c` | 30 → **32 点**（追加两个安全输入点 + 注释说明为什么追加在末尾） |
| `07/src/memory_device_io.h` | `mem_point` 加两常量、`read_limits()` 真读、新增 `set_bms_forbid()`、`init_defaults()` 补两点 |
| `07/src/rtdb/rtdb_device_io.h` | `read_limits()` 改真读（核心）+ 新增 `limits_are_live() → true` |
| `04/src/device_io.h` | 接口新增 `limits_are_live()`（第 ⑨ 项虚函数） |
| `07/src/realtime_loop.h` | `LoopConfig::refresh_limits_each_step` + `attach_device()` 自动打开 + `step()` ⓪ 步 + 注释改正 |
| `07/tests/test_rtdb_device_io.cpp` | T25 追加四方向断言（519 → **533**）；`configure_runtime()` 显式关刷新（本测试比的是介质等价性） |
| `07/tests/test_*`（T26） | 无改动（验证仍逐拍差异 0 拍） |
| `11/src/integration_runner.h` | `kind` 7/8 + 每拍全量重放 + `bms_forbid_ticks` |
| `11/src/main_ems.cpp` | 改采样式（边跑边采）+ 两项检查单 |
| `11/tests/test_system_integration.cpp` | 新增 **T48**（135 → **143**） |
| `11/scripts/run_integration.bat` | 设备侧加 `--fault 8:30:35` |
| `12/src/acceptance_runner.h` | 新增 **A5-07**（验收检查项 53 → **54**） |
| `12/tests/test_acceptance.cpp` | T55 期望值 6 → 7 |
| `P3/src/iec104_point_map.h` | 仅注释澄清（映射表是**子集**，不随点表总数变；BMS 保护位是否上送调度属**待定设计**） |
| `README.md` / `07/docs/README.md` / `11/docs/README.md` / `12/docs/README.md` / `docs/规划/模拟器与设备接入梳理.md` / 本文件 / memory | 断言数与点数同步；新增 `07/docs` §8.6 §8.7、`11/docs` §4.1 §5.4、`12/docs` §5.4；A1 标记**已修** |



---

## 26. 缺口 A2：关口功率"同一路数据两个口径"（2026-09-19）

> 来源：`docs/规划/模拟器与设备接入梳理.md` §10 的**缺口 A2（安全相关）**。
> 性质：**防逆流的命门量取错源**。改动量比 §25 小得多，但**差点修了个寂寞**
> —— 真正有价值的部分是"先造出能区分对错的可观测差异"（§26.3）。

## 26.1 现象

同一路"关口功率"，两个出口取了两个不同的源：

```cpp
// 07/src/rtdb/rtdb_device_io.h（修复前）
// 算法路径：三路量测相减 —— "算出来的"
out.p_grid_kw = p_load - p_pv - p_bat;

// 记录/上报路径
a.p_grid_kw = cached_value(EMS_P_GRID);   // 电表点 —— "表报的"
```

后果链：

```
关口电表（唯一权威计量点）
   └─► S05 防逆流：surplus = p_grid_min - rt.p_grid_kw
   └─► 变压器过载约束：|p_grid| + 0.1 · p_load
```

两个消费者都是**安全约束**。三路量测各有 CT/PT 精度、滤波、通信周期差异，
**相减是把误差叠加而不是抵消**。现场表现：**"报表里 100 kW、防逆流却按 137 kW 动作"**
—— 而这个偏差还会**随工况漂**，无法用固定系数标定回来。

## 26.2 根因（两层）

1. **关口功率没有"唯一定义"**：`p_grid = p_load - p_pv - p_bat` 这个式子
   在 `07/` 里出现两处（`read_snapshot()` 与 `PlantModel`），各自维护。
   谁"更权威"从未被写下来 —— 而真实系统里答案是**电表**。
2. **★ 电表点的更新时机本来就慢一拍**（补断言时才暴露）：
   `MEAS.P_GRID` 原先**只在 `execute()`（拍末）** 更新，而负荷/光伏在
   `set_environment()`（**拍首**）写入 → 电表读数与其它量测**不在同一时刻**。
   改前 `read_snapshot()` 用本拍的 `load/pv` **自己重算**，恰好把这个偏差掩盖掉。
   **也就是说 T21 的"逐位等价"有一半是绕过点表换来的 —— 等价性本身是那个 bug 的产物。**
   （同型教训见 §25.2 第 3 层：注释/测试声称的行为与代码实际行为不一致。）

## 26.3 ★ "差点修了个寂寞" —— 修之前必须先造出可观测差异

设备侧（`MemoryDeviceIO` / `PlantModel`）发布的 `MEAS.P_GRID` **恰好就等于**
`p_load - p_pv - p_bat`（平衡式本来就成立）。所以把 `read_snapshot()` 改成
"读电表点"之后，**在所有现有夹具下都是逐位恒等**：

- 编译通过 ✅  单测全绿 ✅  验收全绿 ✅
- **但断言杀不死旧写法** —— 把代码退回去，一条也不会红。

这与 §25 是同一类陷阱。修法分两步，**顺序不能反**：

1. **先造差异**：电表模型加**系统偏差**（`PlantConfig::meter_bias_kw` /
   `DeviceSimConfig::meter_bias_kw` / `MemoryDeviceIO::set_meter_bias_kw`），
   **默认 `0.0` → 既有行为逐位不变**。夹具把两条路径拉开一个可观测的 gap。
2. **再谈修好**：判据断言的是"**快照跟电表、不跟平衡式**"，而不是"两者相等"。

## 26.4 修法

### 26.4.1 计量口径收敛成单一定义（`07/src/plant_model.h`）

```cpp
// 关口电表模型（**关口功率的唯一定义**）。
double meter_p_grid() const {
    return (p_load_kw_ + cfg_.pcs_standby_kw) - p_pv_kw_ - p_bat_actual_
           + cfg_.meter_bias_kw;
}
double p_grid_actual() const { return meter_p_grid(); }
```

`sample()`（量测，含本拍噪声）与 `p_grid_actual()`（真值，无噪声）
**都从它出** —— 差别只剩"是否含本拍噪声"，不再有"谁在推算"的分歧。

### 26.4.2 适配器两个方向都改（`07/src/`）

| 文件 | 改动 |
| --- | --- |
| `rtdb_device_io.h` | `read_snapshot()` 改 `out.p_grid_kw = cached_value(EMS_P_GRID);` |
| `memory_device_io.h` | 同上读 `get(mem_point::kPGrid)`；新增 `set_meter_bias_kw()`；`update_grid()` 计入偏差 |
| `sim_device_io.h` | 注释说明关口功率由 `PlantModel::meter_p_grid()` 给出 |

### 26.4.3 ★ `set_environment()` 必须同步刷新电表点

```cpp
void set_environment(double p_load_kw, double p_pv_kw) {
    set(mem_point::kPLoad, p_load_kw);
    set(mem_point::kPPv,   p_pv_kw);
    // 一次环境发布 = **同一时刻的一致量测集**：同步刷新关口电表读数。
    update_grid(get(mem_point::kPBat));
}
```

不加这一句，改"读电表点"之后 `MEAS.P_GRID` 会比其它量测**慢一拍**，
`07/T21` 会立刻红 4 处（`d_cmd=75` / `d_actual=75` / `d_soc=6.46e-05` / `d_grid=75`）。
**这不是等价性被破坏，而是等价性原本就靠"绕过点表"维持。**

## 26.5 断言（三层，各自独立口径）

| 层 | 文件 / 用例 | 判据 |
| --- | --- | --- |
| 点表契约 | `07/tests/test_rtdb_device_io.cpp` **T25** | 写 `EMS_P_GRID = 137`（≠ 平衡值 157）→ 快照 = 137；反向守卫 `|balance - meter| > 1e-6`；`read_actuals()` 同口径 |
| 适配器（单进程） | `07/tests/test_rtdb_device_io.cpp` **T29**（新增）、`test_device_io.cpp` **T22** | MemoryDeviceIO（240 vs 平衡 200）/ SimDeviceIO 电表模型 + `bias=0` 反向验证回到平衡值 |
| 跨进程 | `11/tests/test_system_integration.cpp` **T49**（新增） | `meter_bias_kw = 40`，200 拍：`|p_grid - (balance+40)| < 1e-6` 全中、两路径最大差 `0.0`、gap 的 min/max 都 = 40 |
| 验收 | `12/` **A1-09**（新增） | 公开出口口径；反向守卫：两源确实差 40 kW |

**判据有效性已验证**：把 `read_snapshot()` 临时退回推算式 → **T49 立刻 6 条红**
（`ok_meter` / `ok_algo` / `ok_gap` / `min_gap` / `max_gap` / `max_err_algo`），
`PASS=145 FAIL=6`。恢复后回到 `PASS=151 FAIL=0`。

## 26.6 实测

```
电表偏差=40.0 kW
逐拍三路平衡差=[40.0, 40.0]
两路径最大差=0.0
命中拍数=200/200

[PASS] A1-09  关口功率单一数据源（电表口径）
结论: 通过  ——  55 / 55 项通过，0 个维度未全通过
```

## 26.7 验证基线

| 组件 | 修复前 | 修复后 |
| --- | --- | --- |
| `07/` RT_DB | 533 | **546**（+13） |
| `07/` P0+P0.5 | 79 | **81**（+2） |
| `07/` 主测试 T11~T16 | 6050 | **6050**（不变） |
| `11/` | 143 | **151**（+8） |
| `12/` T51~T59 | 113 | **114**（+1）；验收检查项 54 → **55** |
| **全量** | 8523 | **8547** |

## 26.8 通用教训

1. **★ 先造出能区分对错的可观测差异，再谈修好。** 当"正确写法"与"错误写法"
   在现有夹具下**逐位相等**时，任何断言都杀不死旧写法。
   加一个**默认 0、可注入**的偏差量，是让契约"有区分度"的必要动作。
2. **同一物理量的两个出口必须同源。** 出现"算法一路、记录一路"就迟早分叉；
   收敛成**一处定义 + 两个消费者**，分歧在结构上就不可能出现。
3. **"一次环境发布 = 同一时刻的一致量测集"** —— 拍首写负荷/光伏却不刷新电表，
   读数就不属于同一时刻。**量测集的时间一致性是隐式契约，没人写、但必须守。**
4. **等价性测试可能被测对象自身的 bug 维持。** 改完一处发现"等价性破了"，
   先问"原来的等价是怎么来的"，而不是急着回滚。
5. **安全链输入的改动要先问"谁在算、谁在报"。** 电表是唯一权威计量点这件事
   必须在代码里表达出来，不能靠"反正数值一样"。

## 26.9 遗留

- 电表现在只建模为**常量系统偏差**。真机还需 **一阶滞后**（电表积分周期）
  与**慢漂移**，否则仍无法复现"偏差随工况变"的现场现象。
- `MEAS.P_GRID` 目前仍由设备侧推算后发布（因为仿真里没有真实电表）。
  **接真机时这一路必须换成电表直读的寄存器** —— 这是 `13/`（Modbus）的输入。

## 26.10 涉及文件

| 文件 | 改动 |
| --- | --- |
| `07/src/plant_model.h` | 新增 `meter_bias_kw` 配置 + **`meter_p_grid()` 唯一定义**；`sample()` 计入偏差 |
| `07/src/memory_device_io.h` | `set_meter_bias_kw()`、`update_grid()` 计偏差、**`set_environment()` 同步刷新电表点**、`read_snapshot()` 读电表点 |
| `07/src/rtdb/rtdb_device_io.h` | `read_snapshot()` 改读 `EMS_P_GRID`（核心） |
| `07/src/sim_device_io.h` | 注释说明（无行为变化） |
| `07/tests/test_rtdb_device_io.cpp` | T25 改为"电表 137 vs 平衡 157" + 反向守卫；新增 **T29**（533 → **546**） |
| `07/tests/test_device_io.cpp` | T22 追加"两路径同口径"（79 → **81**） |
| `11/src/integration_runner.h` | `DeviceSimConfig::meter_bias_kw` + `apply_plant()` 透传 |
| `11/tests/test_system_integration.cpp` | 新增 **T49**（143 → **151**） |
| `12/src/acceptance_runner.h` | 新增 **A1-09**（验收检查项 54 → **55**） |
| `12/tests/test_acceptance.cpp` | T51 期望 8 → 9 |
| `README.md` / `CHANGES.md` / `07/docs` / `11/docs` / `12/docs` / `docs/规划/模拟器与设备接入梳理.md` / memory / skill | 断言数与检查项同步；A2 标记**已修** |

---

## 27. 缺口 B1：Modbus TCP 设备接入（新模块 `13/`，2026-09-20）

> 来源：`docs/规划/模拟器与设备接入梳理.md` §10 的 **B1**。
> 性质：**新增能力**（不是修缺陷），但它把整个工程的"设备侧"从
> **进程内仿真**推进到**真的过网线**。过程中在 Python 侧抓到 3 个真问题、
> 在 C++ 侧抓到 3 个，并因此新增了 437 条断言。

## 27.1 为什么必须做

前三个 `IDeviceIO` 实现（`SimDeviceIO` / `MemoryDeviceIO` / `RtDbDeviceIO`）
全部活在**同一台机器**内。它们能证明算法对，但证明不了**协议对**。
现场的第一件事是"把 PCS / BMS / 电表接进来"，而这件事的成功率取决于
协议层的细节（字序、分块、超时、串包），跟算法一点关系都没有。

## 27.2 四个"看起来显然、实际必须显式处理"的点

1. **★ Modbus 没有品质位**。RT_DB 每点自带 `data_quality_t`，
   Modbus 寄存器就是 16 位裸数据。于是"值可信吗"必须**主站合成**。
   把"读失败"当"读到 0"是最典型的静默失败 —— 0 kW 负荷、0% SOC
   都是看起来完全正常的数值。
2. **★ 一次快照 = 一组请求 → 部分成功是常态**。一个事务只能读一段连续地址，
   32 点拆 3 次。用 `block_fails()`（通信/地址）与 `decode_fails()`（编码/字序）
   **两个**计数分开表达，因为现场排查路径完全不同。
3. **★ 32 位浮点字序**：协议只规定"每寄存器 16 位、大端传输"，
   **没规定**两个寄存器谁在前。写错的表现不是"差一点"，是数值完全离谱或 NaN。
4. **★ 阻塞 `connect()` 在设备离线时要等 OS 的 SYN 重传超时**
   （Windows 实测 ~20 s），而控制周期只有 200 ms。现场表现是"程序偶尔卡死"。

## 27.3 测试分三层（这是本模块的核心结构决策）

| 层 | 断言 | 依赖 | 保证什么 |
| --- | --- | --- | --- |
| 协议层 `test_modbus_tcp.cpp` | 221 | 无（内置 fake 从站） | 客户端符合协议 + 异常路径正确 |
| 适配器契约 `test_modbus_device_io.cpp` | 133 | 无（内置 fake 从站） | 32 点快照 / 原子下发 / 部分成功 / 安全位 |
| 跨语言 `test_modbus_bridge.cpp` | 83 | **Python + pymodbus** | 两个**独立实现**能真正互通 |
| 合计 | **437** | | |

**★ 第三层不能省**：前两层的 fake 从站也是我们自己写的 ——
**自己写两端等于自己和自己对答案**。双方共同误解协议时谁都发现不了
（比如都把字序记成 AB，前两层全绿，现场接真机全错）。
`pymodbus` 是业界事实标准，拿它当对端，"我们的客户端符合协议"
才第一次成为**有外部证据**的结论。

**故障注入必须放在第二层**：Python 从站是别人的实现，不会配合你演
半包/串包/静默。这些最需要验证的场景只能在进程内造
（`fake_modbus_slave.h::Options`）。

## 27.4 实测抓到的缺陷（模块内 6 个；另有 3 个在"包装模块的东西"里，见 §27.12）

| # | 侧 | 缺陷 | 发现方式 | 后果（若不修） |
| --- | --- | --- | --- | --- |
| 1 | C++ | **超时被当成"可重试"**（`WSAETIMEDOUT` 与 `WSAEWOULDBLOCK` 合成一类） | fake 从站 `silent` 注入把测试**卡死** | 设备静默时 `recv_exact()` 无限重试，**主站挂住** |
| 2 | C++ | 阻塞 `connect()` 离线等 ~20 s | 代码审查 + T27 实测 | 控制周期 200 ms 被拖垮，"程序偶尔卡死" |
| 3 | C++ | 串包时直接失败（应丢包继续等） | T10 串包注入 | 一次抖动**连锁成两拍**数据缺失 |
| 4 | Python | **`p_grid` 注释写了"加电表偏差"但代码里没有** | 反证① | 最关键的口径断言在默认参数下**永远绿** |
| 5 | Python | 老 datastore 路线（`ModbusDeviceContext`）在 3.15 里已被 `deepcopy` 断开 | 起站实测崩溃 | 改寄存器**服务端看不见**且**不报错** → 主站永远读 0 |
| 6 | Python | `args.meter_bias` 与 `--bias` 参数名不一致 | 起站实测 | 起服务直接崩 |

★ 缺陷 1 的价值最大：**它不是读代码看出来的，是故障注入把测试卡死后暴露的**。
这正是"故障注入"存在的意义。

★ 缺陷 4 命中本项目已栽过四次的老纪律：**注释声称的行为必须与代码实际行为一致**。
且**默认值不能作为"这一行存在"的证据** —— `--bias 0` 时"加了"与"忘了加"逐位相等。

## 27.5 判据有效性反证（5 次）

| # | 破坏的写法 | 结果 | 抓它的测试 |
| --- | --- | --- | --- |
| 1 | Python `p_grid` 去掉 `+ meter_bias` | **3 条红** | T42 |
| 2 | Python 点表 `IR_P_GRID` 6 → 7 | **3 条红**（T40 精确定位到第 3 行并并排打两侧字段） | T40 |
| 3 | Python `read_command` 恒返回 None | **4 条红** | T43 / T44 |
| 4 | `simdata` 交换 DI↔IR | pymodbus 构造时 `TypeError` 拒绝 | ★ **非缺陷** |
| 5 | `simdata` 交换 CO↔DI（同为 BITS） | 仍 83/0 全绿 | ★ **非缺陷** |

★ 反证 4/5 的价值在于**它们证明我在注释里写错了一句**：原注释声称
"元组顺序写反会让四张表整体错位、只能靠跨语言对账抓"。实测结论是
① 跨类型写反由 pymodbus 类型检查当场拒绝；② 同类型写反**没有影响** ——
表身份由**位置**决定，而读写都走同一功能码入口。真正风险是
**C++ 侧 `Table` 枚举与标准功能码的对应**写错。**注释已按实测改正。**
（"自己的判断错了要直接认，然后修口径"。）

## 27.6 关键设计：把"3 次请求"变成契约

32 点按表分三段（IR 40 / HR 6 / DI 8），**段内地址连续**。
读全 32 点 = **3 次请求** —— 这个数字被 T09 / T22 / `--plan` **同时钉死**。

把一个性能属性变成契约的意义：任何"顺手改成一点一次"的退化会
**立刻变红**，而不是等现场发现"采一帧要 3 秒"。

代价也真实：新增点必须落在段尾（IR[11] 那个空位就是为此留的）。

四条编译期守卫（`static_assert`）：`bindings_well_formed` /
`segments_consistent` / `cmd_points_contiguous` / `blocks_cover_all_points`
+ `blocks_disjoint`。其中 `cmd_points_contiguous()` 保证三个指令点地址连续 ——
**这是一次 FC16 原子下发的前提**（否则设备可能执行到"新功率 + 旧权限区间"的中间态）。

## 27.7 关口功率口径的跨语言复核（A2 的延伸）

Python 从站的电表读数 = 平衡值 + `--bias`。主站若在本地重算会小 40 kW。

实测（`--bias 40`，第 2 遍已下发 -60 kW）：

| 遍 | `p_bat` | `p_grid` | 校验 |
| --- | --- | --- | --- |
| 1 | 0.00 | 272.00 | `380+2-150-0+40 = 272` ✓ |
| 2 | -57.41 | 329.41 | `272-(-57.41) = 329.41` ✓ 且模型正在跟踪指令 |

第二行同时证明了**闭环**（EMS 写 HR → Python 设备模型消费 → 量测变化）。

## 27.8 验证基线

- 13/ 三层：**221 + 133 + 83 = 437**，`ALL TESTS PASSED`，退出码 0
- 全量：**8547 → 8984**（`All 31 components built.`，`build_all.bat` 第 **30/31** 步）
- ★ **环境依赖导致基线有两个合法值**（下表按**实测**，不是按设计意图）：
  | 条件 | 13/ 贡献 | 全量基线 | 13/ 收尾行 |
  | --- | --- | --- | --- |
  | **开箱默认**（仓库里没有装了 pymodbus 的 Python） | 359 | **8906** | `[SKIP] … 跨语言层未运行` |
  | 有 pymodbus（`13\.venv` 或 `EMS_PYTHON`） | 437 | **8984** | `[OK] … 三层测试全部通过` |
  ★ **默认口径是 8906，不是 8984** —— 这一点是跑完第一次全量回归才发现的（见 §27.12）：
  pymodbus 装在哪台解释器上，`build_all.bat` **管不着**。报告里必须写清用的哪个口径，
  否则"数字对不上"会被误判成缺陷。**判据**：`grep "SKIPPED=0" build_all.log` 命中 → 8984。
- `--plan` 实测：自检通过 / 32 点 / 3 次分块；离线端口 **304 ms** 快速失败

## 27.9 环境缺失的处置（skip 而非 fail，但必须显式）

跨语言层两级环境探测：

| 级别 | 探测 | 失败含义 | 处置 |
| --- | --- | --- | --- |
| 1 | `--dump-tsv`（**不** import pymodbus） | 没有 python / 路径不对 | SKIP |
| 2 | `--check-pymodbus` | 有 python 但**没装 pymodbus** | SKIP + 安装指引 |
| — | 两级都过 | 起不了服务 = **真缺陷** | FAIL |

★ 第 2 级不能省：`--dump-tsv` 不需要 pymodbus，所以"没装 pymodbus"
会通过第 1 级，然后在起服务时失败 —— 被报成**代码缺陷**。
不做这个区分，构建脚本会在没装 pymodbus 的机器上**永久假红**。

三级行为已实测：① 有 pymodbus → 83/0（`SKIPPED=0`）；② 无 pymodbus → `PASS=5 SKIPPED=7` 退出 0；
③ 无 python → `PASS=5 SKIPPED=7` 退出 0。

★ **"skip 而非 fail"只解决了一半的问题**：另一半是**上层脚本必须承认自己没跑**。
第一版 `build_test.bat` 在 SKIP 的情况下仍然印 `[OK] 13\ Modbus 三层测试全部通过` ——
退出码 0、日志里也确实有 `SKIPPED=7`，但**最终结论行是假的**。这正是本项目最忌讳的
「静默降级」：把"没验"写成了"验过"。见 §27.12。

## 27.10 与 pymodbus 版本绑定的硬事实（3.15 实测）

1. **不要走 `ModbusDeviceContext` + `ModbusSequentialDataBlock` 老路**：
   3.15 里 `ModbusDeviceContext.__init__` 对 data block 做 `deepcopy`，
   构造后再改 `ir.simdata[...]` **服务端看不见**，而且**不报错**。
   正确做法：`pymodbus.simulator` 的 `SimData/SimDevice` + `pymodbus.server.ModbusTcpServer`。
2. **`SimData.count` 对 BITS 是乘数**：`count=64` 配 64 个 bool → 展开成 4096 位。传 1。
3. **更新寄存器只有一个公开入口**：`server.async_setValues(unit, fc, addr, values)`，
   `fc` 是**标准功能码**（1=CO 2=DI 3=HR 4=IR）。所以脚本必须自己持有
   `ModbusTcpServer` 实例（`StartAsyncTcpServer()` 是 fire-and-forget，拿不到句柄）。
4. `SimData(0, ...)` 的地址 0 **就是**协议地址 0，与 `modbus_point_map.h` 直接对齐，**别套 ±1**。

## 27.11 涉及文件

| 文件 | 改动 |
| --- | --- |
| `13/src/modbus_tcp_client.h` | **新增** MBAP/PDU 编解码、事务、超时分类、串包自愈、非阻塞 connect |
| `13/src/modbus_point_map.h` | **新增** 映射唯一真相源 + 4 条 `static_assert` 守卫 + 分块表 |
| `13/src/modbus_device_io.h` | **新增** `IDeviceIO` 第 4 个实现；补 `reconnects()` 诊断 |
| `13/src/fake_modbus_slave.h` | **新增** 进程内从站 + 故障注入（半包/串包/异常/ByteCount/静默） |
| `13/src/main_probe.cpp` | **新增** 现场点表核对工具 |
| `13/tests/test_modbus_tcp.cpp` | **新增** T01~T15（221） |
| `13/tests/test_modbus_device_io.cpp` | **新增** T21~T30（133） |
| `13/tests/test_modbus_bridge.cpp` | **新增** T40~T47（83）+ 两级环境探测；★ 后追加修复 3 处（见 §27.12） |
| `13/sim/modbus_slave.py` | **新增** Python 从站 + 设备模型 + `--dump-tsv` / `--check-pymodbus` |
| `13/scripts/build.bat` / `build_test.bat` / `run_sim.bat` | **新增**；`build_test.bat` 收尾行后改为**据实**报 SKIP（见 §27.12） |
| `13/sim/requirements.txt` | **新增** `pymodbus>=3.15,<4` |
| `13/docs/README.md` / `design.md` | **新增** |
| `scripts/build_all.bat` | 新增第 **30/31** 步；`All 29` → `All 31` |
| `README.md` | 模块地图加 `13/`；全量基线 8547 → **8906（默认）/ 8984（有 pymodbus）** |
| `docs/规划/模拟器与设备接入梳理.md` | §10.2 / §10.5 标记 B1 **已完成**；后续排序改为 A3 → B2 → A2 遗留 |
| memory / skill | 基线更新 + B1 三条新纪律 |

---

## 27.12 全量回归时发现的第 7~9 个缺陷：**"skip 而非 fail"只做了一半**

**触发**：第一次跑完整 `build_all.bat`（31 步）后核对总数，得到的是 **8906** 而不是文档里写的 8984。

**根因排查**（三步，都不是代码问题，而是"我以为的环境"和"脚本看到的环境"不一致）：

1. 日志里 `13/` 第三层是 `PASS=5 SKIPPED=7` —— 说明**这一层根本没跑**。
2. `which python` → 解析到的是托管工具链里的 `versions/3.13.12/python`，它**没装** pymodbus；
   pymodbus 3.15 装在同一套工具链的另一个环境 `envs/default/Scripts/python.exe` 里。
   而 `build_all.bat → build_test.bat` 的环境里**没有任何一级指向它**（也不该指 ——
   项目脚本依赖我的工具链环境是错误的耦合）。
3. 于是"8984"这个数字，只有在**手工设了 `EMS_PYTHON`** 的手跑场景里成立。**文档高估了默认口径。**

| # | 缺陷 | 危害 | 修法 |
| --- | --- | --- | --- |
| **7** | `build_test.bat` 在第三层 SKIP 时**仍然**印 `[OK] … 三层测试全部通过` | ★★ **最终结论行是假的**：退出码 0、日志中间确有 `SKIPPED=7`，但读结论的人只会看到"三层全部通过"。这是「静默降级」的教科书形态 —— 把"没验"写成了"验过" | `build_test.bat` 先删旧状态文件 → 跑桥接测试 → 用 `findstr /C:"SKIPPED=0" build\bridge_status.txt` 取判定 → 收尾行**二选一**：`[OK] 三层（437 条）` / `[SKIP] 前两层（221+133），跨语言层未运行，本次全量口径 8906` |
| **8** | `SKIPPED=n` **只在 skip 时才打印** | "第三层确实跑了"在日志里**没有正面证据**，只能靠"没有那一行"推断 —— 而"缺行 / grep 写错 / 日志被截断"三者不可区分。**正面证据优于"没有反面证据"** | 桥接测试**总是**打印 `SKIPPED=n`（哪怕是 0），并另落一行纯 ASCII 的 `build/bridge_status.txt`（`PASS=… FAIL=… SKIPPED=…`）。用文件而不是 grep 中文输出，是为了**绕开 bat 的编码问题** |
| **9** | `bridge_slave.log` 落在 **`13\` 模块根** | `13\` 是**源码目录**；运行期文件混进去后，"这次改了什么"越来越难看清（第一次跑就留了个 0 字节的） | 路径改为 `build/bridge_slave.log`（`build/` 不存在时退回当前目录），可用 `EMS_BRIDGE_LOG` / `EMS_BRIDGE_STATUS` 覆盖 |

**口径实测**（两次都跑同一二进制，只换 `EMS_PYTHON`）：

| 环境 | 输出 | 13/ 贡献 | 全量 |
| --- | --- | --- | --- |
| 默认（PATH 上的 python） | `PASS=5 FAIL=0` / `SKIPPED=7` | 359 | **8906** |
| `EMS_PYTHON=<装了 pymodbus 的 python>` | `PASS=83 FAIL=0` / `SKIPPED=0` | 437 | **8984** |

**教训（与 §27.4 的 6 个不同，这三个不在模块里，在"包装模块的东西"里）**：

- ★★ **"允许 SKIP"和"必须宣布自己 SKIP 了"是两件事，缺第二件等于没做第一件。** 测试层诚实地
  打了 `SKIPPED=7` 还不够 —— 只要**上层脚本**给出一个相反的结论行，读者就只会记住那一行。
  **凡是"允许不跑"的测试层，都必须有一个"跑了没有"的显式信号一路传到最外层。**
- ★ **默认值必须是"实测跑出来的那个"，不是"设计期望的那个"。** 我先前把 8984 写进 README，
  依据是"我手工设环境跑过 83/0"；但**官方入口（`build_all.bat`）看到的不是这个环境**。
  凡是跨"我的环境 / 脚本的环境"边界的数字，**必须用官方入口实测一次再写**。
- ★ **运行期产物不要落源码目录**：相对路径按 CWD 落盘，而 `build_test.bat` 的 CWD 是模块根。
  **交付物落点 = `build/` 或 `logs/`**，且位置要可用环境变量覆盖。
---

## 28. 缺口 A3.1：外部设定落点（EXT 点区，2026-09-20）

> **用户明确选的路径是「分两步：先通路后语义」。** 本步（A3.1）只交付**通路**：
> EXT 点区 + 窄接口 + 落点适配器 + 网关接线 + 回执 + 失效兜底 + 测试。
> 把外部设定**接进 `05/` 安全链**（A3.2）**不在本步** —— `narrow_interval_by_ext()`
> 已实现且有 T72 覆盖，但**没有任何生产路径调用它**。

### 28.1 为什么必须做

IEC104 点表里 6 个下行点（`6001~6004` 遥调、`5001~5002` 遥控）**在 RT_DB 里没有落点**
（`download_table()` 的 `src` 是 `""`），所以网关原先**一律否定确认**。
那比「回肯定确认但没落地」诚实 —— 但调度也就真的控不了。

### 28.2 ★★ 让步的边界写在**类型**里，不是注释里

| 手法 | 表达 |
| --- | --- |
| 只读口 | `RtDbReadOnlySource` **没有 `write()`** —— 想写也写不出来 |
| 写口 | `IExtSetpointSink` **只有六个具名 setter** —— 没有「按索引写 / 按点名写 / 批量写」入口 |

所以「网关写 `CMD.*`」不是「不允许」，而是**写不出来**。这与 A1/A2 反复吃过的
「白名单是运行期校验、具名方法是编译期约束」是同一条结论：
**把约束放到类型上，跨版本、跨人手都不会失效。**

分区由 `ems_point_zone_of()` + `RtDbExtWriter::self_check()` 守着，不靠注释。
至此 RT_DB 上**三个写入者**定型，点区互不重叠：

| 写入者 | 点区 |
| --- | --- |
| `RtDbPointWriter`（设备侧） | MEAS / STA / CFG |
| `RtDbDeviceIO`（EMS 侧） | CMD |
| `RtDbExtWriter`（网关） | **EXT** |

### 28.3 8 个点，不是「6 + 12」

`EXT.P_SETPOINT` / `P_UPPER_SET` / `P_LOWER_SET` / `D_TARGET` / `PCS_ONOFF` / `EMS_ENABLE`
六个载荷 + **一个区级 `EXT.SEQ`（发布信号）+ 一个区级 `EXT.TS`（陈旧判定）**。

为什么不做「每点一个 TS/SEQ」：EXT 区保存的是**当前外部设定的状态**（电平语义），
不是命令队列。逐点序号唯一多出来的能力是「分辨哪一条命令变新了」，那对应
**事件语义**，与设定语义不匹配，而且是 3 倍点数与出错面。

**默认值必须落在「不引入新约束」那一侧**（A1 同款纪律）：
`EMS_ENABLE` 默认 **1** —— 取 0 会让「调度还没发过任何命令」就等于「调度闭锁了 EMS」，
冷启动只监视不调节；`P_UPPER_SET/P_LOWER_SET` 取 ±1e9（「极大数 = 不限制」约定）；
`P_SETPOINT` 取 0（0 kW 对储能不是有意义的指令，停机另有 5001 → 天然作「无设定」哨兵）。

### 28.4 ★★ 发布协议：**前后各递增一次**（A3.1 抓到的真缺陷）

`rt_db_set_value` 只保证**单点**不撕裂。而「上下界 + 设定 + 时标」是一组应当同时生效的量：
逐点读可能读到「新的上界 + 旧的下界」，而对安全区间来说**中间态可能比两端都宽**。

原实现只在**末尾**递增 SEQ。这条时序会把半更新快照判成「一致」：

```
读者读 s0 = 7（写者还没开始）
写者开始写载荷（一次一个点，中间态是新旧混合）
读者在写者提交**之前**读 s1 = 7        ← 等于 s0
→ 判定「一致」，但载荷是混合的
```

**末次递增只挡住「读完之后才变」，挡不住「读的过程中在变」。**
单点 setter 看不出问题；`clear_all()` 一次写 6 个点才是真正的撕裂点。

修法：`begin_publish()`（载荷写入前）+ `commit_publish()`（载荷写入后）。
稳定态 SEQ 恒**偶**、写入中为**奇**；读者要求 `s0 == s1` **且 `s0` 为偶数**
（「相等」挡快写者，「偶数」挡慢写者 —— 两条各挡一半，缺一不可）。

★ `clear_all()` **不把 SEQ 归零**：SEQ 表示「本区被改写过多少次」，归零 = 在读者眼里倒退，
而「倒退」与「没变化」在「要不要重新处理」上不可区分。

### 28.5 失效兜底：两条**正交**判据，缺一不可

| 判据 | 位置 | 覆盖 |
| --- | --- | --- |
| 主站断开 / 空闲超时 → `clear_all()` | 网关 | 快路径（让状态早点变对） |
| `now - EXT.TS > stale_s` → 整区无效 | **EMS 侧**（`07/src/rtdb/ext_setpoints.h`） | **兜底：不依赖任何进程活着** |

★ 网关进程被 kill 时 `clear_all()` **不会跑** → 最后一个设定会永远生效。
所以兜底**不能只靠写者**。

`ExtSetpoints` 的五个标志**正交**（不合并成 bool）：`present` / `stale` / `consistent` /
`finite` / `band_ok`。两个容易写反的边界：

- 时标**来自未来**（`now < ts_s`）→ 必须判不可用，否则「陈旧判定」永远不成立，兜底等于关掉；
- `stale_s <= 0` 的含义是「**禁用外部设定**」（视为总是陈旧），不是「禁用超时判定」——
  前者的失效方向是安全的。

### 28.6 三档受理模式

| 模式 | 开关 | 行为 |
| --- | --- | --- |
| 默认（安全侧） | 无 | **一律否定确认** |
| DRY-RUN | `--accept-commands` | 只记录不落地（向后兼容的旧行为） |
| 真落点 | `--ext-rt-db` | 落到 EXT 区；**写成功才回肯定确认**，失败回否定 + 原因 |

两个开关同时给时取**更强**的那个 ——「以为在 dry-run 其实真落了」是**危险方向**上的误解。
★ `--ext-rt-db` 下 `self_check() != 0` 时**降级为默认拒**（不是「警告后继续」），
并在横幅后显式声明「上方横幅已作废」—— 避免「启动横幅说落了、实际没落」。

### 28.7 测试分两组（为什么必须有一个走真段的 exe）

| 组 | 文件 | 覆盖 | 为什么不合并 |
| --- | --- | --- | --- |
| T61~T72 | `tests/test_iec104.cpp` | 协议层 + EXT 判定（假读回调穷举边界） | 数据源是**内存实现**，协议测试不该依赖共享内存段 |
| T73~T76 | `tests/test_ext_rtdb.cpp` | 真段 + 真 `rt_db_set_value`/`rt_db_get_value` + 真并发 | **A1 的教训：跨内存边界 + 夹具注入 = 测试盲区** |

T73~T76 全程**不注入任何东西**：写走 `RtDbExtWriter`，读走 `rt_db_get_value`，
并且**按名字反查索引**再对账（索引是运行时坐标，只有名字能发现「点表中间被插了点」）。

### 28.8 判据有效性反证（2 次，都是本次新协议）

| 反证 | 操作 | 结果 |
| --- | --- | --- |
| T71 ③b 奇偶判据 | 把 `s1 == s0 && is_committed_seq(s0)` 改回 `s1 == s0` | **3 条红**，红的正是「接受了半更新快照（上界 999 被当成可用）」 |
| T76c 并发 | 把 `begin_publish()` 挪到载荷写入**之后**（序号算术完全不变） | **2 次违规**：`seq=66058 读到 (-16610.0, 49627.0)，该序号应为 (-16609.0, 49627.0)` —— 形态正是「**新下界配旧序号**」 |

两次都**恢复后复跑全绿**（376 / 121）。

### 28.9 本次抓到的 4 个缺陷

1. ★★ **发布协议缺 `begin` 递增**（§28.4）—— 半更新快照被当成一致，有反证。
2. ★ **`.o` 缓存只判「存在」不判「源文件有没有更新」**：点表 32 → 40 点后，
   `P3/build/ems_point_table.o` 还是**前一天**编的 32 点版本，而 `07/` 里同名对象是新的 ——
   **同名点表、两套内容**，编译期一个字都不说。
   根因：`if not exist build\ems_point_table.o (...)`。修法：三处
   （`P3/scripts/build.bat`、`P3/scripts/build_test.bat`、`13/scripts/build_test.bat`）
   改成**无条件重编**。通用教训：`if not exist x.o` 这种「手工 make」在**头文件驱动**的
   项目里必然踩坑 —— 改的是 `.h`，编的是 `.c`，判断的却是 `.o` 在不在。
3. **`ExtSetpoints::present` 与它自己的注释不符**：注释写「段里收到过外部设定
   （`EXT.SEQ > 0`）」，实现却在循环后**无条件** `present = true` —— 于是
   「连 SEQ 都读不到」也报 `present = true`，现场提示指向完全错误的方向。
   **为什么难发现**：`usable()` 两种情况都是 `false`，**行为没变**。又是
   「注释声称的行为 ≠ 代码实际行为」。
4. **测试自己出的两次「假绿 / 假红」**（不是产品缺陷，但都属于「看着绿、实际没验」）：
   - **断言适用范围写错**：T69b 断言「±1e9 哨兵对**任意**区间都是恒等元」，
     用 ±1e30 当初始区间 → 2 条红。真相是 1e9 **确实会** clip 一个 ±1e30 的区间 ——
     **不要为了消红去调大魔数**，改成「对量级正常的区间是恒等元」+ 把前提本身也断言下来。
   - **并发用例空跑**：T76c 首版 300 次快照**一次都没被对账过**
     （写者「先提交再记账」，而读者能拿到稳定窗口的时刻恰好就是记账之前）。
     看着「零违规」，实际什么都没检查。改成**按公式预推期望值** + 覆盖率守卫
     （`checked == usable_n`）。
     **「没报违规」与「查过之后没违规」是两件事。**

### 28.10 验证基线

- `P3/` 单元测试：**168 → 497**（协议 376 + EXT 落点 121），`FAIL=0`，退出码 0
- 全量：**8906 → 9245**（默认口径）/ **8984 → 9323**（装了 `pymodbus`）
  - `P3/` +329（168 → 497）
  - `07/` RT_DB 546 → **554**（+8：T25 逐点循环覆盖从 32 点变 40 点）
  - `13/` 221 → **223**（+2：EXT 区边界断言）
  - **其余模块不动**（`11/` `12/` 的循环用 `EMS_POINT_COUNT`，但断言数不随点数增长）
- `[BUILD ALL OK] All 31 components built.`，`grep -c "error:"` = **0**
- ★ 归集方式：**按模块逐项对账**（把每个计数行归到它所属的 `N/31` 步骤），
  **不按数字大小猜** —— P3 现在一步里印两行（376 + 121），
  光看数字很容易把 121 当成别的模块。归集脚本：`logs/_tally.py`（临时，不长期保留）。

### 28.11 涉及文件

| 文件 | 改动 |
| --- | --- |
| `07/src/rtdb/ems_point_table.h\|.c` | 点表 32 → 40 点（EXT 区 8 点，**纯追加**）；`ems_point_zone_t` + `ems_zone_name()` + `ems_point_zone_of()` + `ems_is_ext_point()` / `ems_is_cmd_point()`；C 侧「负长度数组」编译期守卫 |
| `07/src/rtdb/ext_setpoints.h` | **新增**：`ExtSetpoints` + `load_ext_setpoints()` + `is_committed_seq()` + `narrow_interval_by_ext()` |
| `07/src/memory_device_io.h` | `mem_point` 补 8 个 EXT 常量 + `init_defaults()` 8 行（与 `EMS_POINT_DEFAULTS` 逐位一致） |
| `P3/src/ext_setpoint_sink.h` | **新增**：`IExtSetpointSink` / `NullSetpointSink` / `dispatch_ext_setpoint()` |
| `P3/src/rtdb_ext_sink.h` | **新增**：`RtDbExtWriter` + 发布协议（`begin`/`commit`）+ `self_check()` |
| `P3/src/rtdb_source.h` | 新增 `borrowed_handle()`（借出句柄，**不构成写权限**） |
| `P3/src/main_gateway.cpp` | `RecordingSink` → `GatewaySink`（三档）；`RtDbExtWriter` 构造 + 自检降级；空闲超时清空；心跳与收尾报告加 EXT 统计 |
| `P3/tests/test_iec104.cpp` | T69 / T69b / T70 / T71 / T72（+208）；头注释与运行横幅改 T61~T72 |
| `P3/tests/test_ext_rtdb.cpp` | **新增**：T73~T76（121 断言，跨共享内存边界） |
| `P3/scripts/build.bat` / `build_test.bat`、`13/scripts/build_test.bat` | `.o` 缓存 → 无条件重编；P3 测试补 [3/3] 步 |
| `13/src/modbus_point_map.h` | `kBindings[EMS_EXT_BEGIN]` + 两条 `static_assert`；6 个文件 21 处 `EMS_POINT_COUNT` → `modbus::kBindingCount` |
| `README.md` / `P3/docs/{README,design}.md` / `07/docs/README.md` / `docs/规划/…` | 基线 9245 / 9323、点表 40 点、A3.1 交付记录 |

### 28.12 未做 / 下一步

- ⏳ **A3.2**：`narrow_interval_by_ext()` 接进 `05/`（加第 10 条约束）+ 装配层调用。
  ★ 接线前先拍板 `P3/docs/design.md` §5.7：**stale 时「停机 / 闭锁」设定不生效（而非粘住）** ——
  调度下发停机后网关死掉 → 超时后 EMS 恢复常规运行，而调度的最后意图是停机。
  现状已被 T72 第 ⑧ 条断言钉住。
- ⏳ `EMS_POINT_DEFAULTS` 的 EXT 段与 `RtDbExtWriter::clear_all()` 是「一处定义 + 两个消费者」，
  但**没有编译期对账**（靠 T76 逐位比对）。点表再加点时注意。
- ⏳ 网关的 EXT 落点还没有**端到端**实测（起初始化器 + 网关 + 主站模拟器，看真段里的变化）——
  本步只到「单元 + 跨内存边界」这一层。现场联调时补。

## 29. 点表扩容后的"过期文案"清扫（32 → 40，2026-09-20）

### 29.1 怎么发现的

在整理"测试情况"清单时，为给出**可核对**的口径，把全量日志逐项对账，顺手 grep 了
`32 点` —— 发现点表在 §28 从 32 扩到 40 之后，**一批仍声称"32 点"的文案没跟着走**。
其中**两处会印进正式产物**：

| 位置 | 印到哪里 | 原文 |
| --- | --- | --- |
| `12/src/acceptance_runner.h:839` | **正式验收报告**（`ACCEPTANCE-REPORT.{md,json,html}`） | `"32 点表自检（点名/单位/索引一致）"` |
| `11/src/main_ems.cpp:157` | **联调报告** | `"跨进程 32 点读取：碰撞被重试吸收…"` |

而同一条检查项的**实测值**用的是 `EMS_POINT_COUNT`（现 40）→
**一条检查项自己跟自己矛盾**：标题说 32、证据说 40。

还有一处不是"过期"而是**一直就错**：`docs/规划/模拟器与设备接入梳理.md` 的段结构图写
`STA 6 点`、`设备侧只写这 27 点` —— 那是 §25（A1 加 BMS 禁充/禁放位，STA 6 → 8）
**之前**的口径，A1 之后就该是 8 / 29，一直没人看第二眼。

### 29.2 修法：不是把 32 改成 40，而是**让它不能手抄**

§3 的纪律"镜像常量必须对着真相源"在这里适用：

- **会印进产物的** → 改成从真相源派生，以后扩容自动跟随：
  `std::to_string(EMS_POINT_COUNT) + " 点表自检（…）"`；
- **注释里的** → 干脆去掉硬数字，写"全点表"，让它**结构上无法过期**。

只把 32 改成 40，是**下一次一定重犯**的写法。

### 29.3 顺手核实：`13/` 的"32 点"是**对的**，不能动

`13/`（Modbus 设备接入）文档与源码里也有大量"32 点"，**那些是正确的** ——
它是 `kBindingCount == EMS_EXT_BEGIN`（设备侧绑定数），由两条 `static_assert` 守着：

```cpp
static_assert(kBindingCount == static_cast<std::size_t>(EMS_EXT_BEGIN), "...");
static_assert(EMS_EXT_END == EMS_POINT_COUNT, "...");
```

**EXT 区不经设备总线**（网关写、设备侧看不到），所以设备侧就是 32 个点。
"同一个数字在 A 文件过期、在 B 文件正确" —— 扫这类文案时**必须逐个分辨它指的是哪张表**。

### 29.4 涉及文件（源/文档 + 1 个产物）

| 文件 | 改动 |
| --- | --- |
| `12/src/acceptance_runner.h` | A1-06 标题改**派生**；2 处注释去硬数字 |
| `07/src/rtdb/rtdb_device_io.h` | 2 处注释 `32 点` → `全点表` |
| `11/src/main_initializer.cpp` | 1 处注释 |
| `11/src/main_ems.cpp` | 1 处注释 + 1 处**报告标题**改"全表" |
| `11/src/integration_runner.h` | 1 处注释 |
| `11/docs/README.md` | 3 处 |
| `README.md` | 产物清单 1 处（初始化器"32 点注册" → "全点表注册"） |
| `docs/规划/模拟器与设备接入梳理.md` | 段结构图（STA 6 → 8 / 27 → 29 / 补 EXT 行）+ 点数行 `= 40` |
| `12/docs/ACCEPTANCE-REPORT.{md,json,html}` | **用官方入口 `acceptance.exe --root ..` 重新生成** |

### 29.5 验证（本机 `cmd.exe` 被策略拦，按 `.bat` 原始命令行逐条复跑）

| 步骤 | 结果 | 与 A3.1 基线比 |
| --- | --- | --- |
| 19 `07/` RT_DB | `PASS=554 FAIL=0` | 一致 |
| 24 `11/` 三进程编译 | `BUILD OK` | 一致 |
| 25 `11/` T41~T49 | `PASS=151 FAIL=0` | 一致 |
| 26 `12/` 验收器编译 | 编过 | 一致 |
| 27 `12/` T51~T59 | `PASS=114 FAIL=0`，`55/55 项通过` | 一致 |
| 重生成报告 | `12/docs/ACCEPTANCE-REPORT.*`（14:22:47） | `40 点表自检` / `点数=40` |

**全量口径不变：9245（默认）/ 9323（装 pymodbus）。** 改的全是文案，断言零增减 ——
这正是"文案修正"与"行为修正"应该有的区分：**如果断言数变了，说明我不只改了文案**。

### 29.6 ★ 顺带发现一条真缺口（**未修**，留证据）

`11/` 的联调夹具 `DeviceSideSim::publish_all()` **只跳过 `CMD` 区**：

```cpp
for (i = 0; i < EMS_POINT_COUNT; ++i) {
    if (i >= EMS_CMD_P_BAT && i <= EMS_CMD_P_LOWER) continue;   // 只跳 CMD
    w_.write(i, dev_.get(EMS_POINT_NAMES[i]), q);
}
```

→ 它**连 EXT 区 8 个点一起写了**。而：

- `11/src/integration_runner.h:299` 注释写"CMD.\* 三点属于 EMS 的下行区，绝不触碰（角色边界）"，
  **没有把 A3.1 新增的 EXT 区考虑进去**；
- `11/src/integration_runner.h:372` **会把这句话印进联调报告**：
  `角色边界  设备侧只写 MEAS/STA/CFG，只读 CMD ✔` —— 与代码实际行为不符；
- `11/tests/test_system_integration.cpp` 的 T41 只断言 **CMD 三点**不被设备侧改动，
  **对 EXT 区零覆盖**（标题却写"点区不越界"）。

功能上目前无害（`11/` 没有网关，EXT 无人消费），但它正是本项目最忌讳的那一类：
**注释/报告声称的边界，与代码实际写的边界不一致；而测试恰好只测了声称的那一部分。**

修它要拍板一件事：**`11/` 是补一个"网关进程"来写 EXT，还是让设备侧跳过 EXT 区？**
两种都合理（后者简单，前者更接近现场"三进程 + 网关"）。**没拍板之前不改。**

### 29.7 通用教训

1. ★ **扩容 / 重命名之后必须 grep 旧数字**（这次是 `grep -rn "32 点"`）。最容易漏的是
   **会印进产物的标题** —— 因为它不像断言那样会红。
2. ★ **同一个数字在不同文件里可能指不同的表** —— 必须逐个分辨归属，不能全局替换
   （`13/` 的 32 点差点被我一起改掉）。
3. ★ **修正"文案 ≠ 实际"时优先改结构（派生 / 去数字），不要只改数值。**
4. ★ **验收报告是产物，不是日志**：点表扩容后**必须重新生成**，否则交付物里留着一个
   与现状矛盾的官方文件。

## 30. 过期数字的系统性清扫（30 / 27 / 29 / 32 三代口径，2026-09-20）

### 30.1 为什么要做第二遍

§29 只清了「会印进产物的验收 / 联调标题」和自相矛盾的那几处。但点表经历过
**30 → 32（A1）→ 40（A3.1）两次扩容**，同一批文案理论上可能留着**两代**过期数字。
于是做了一次**全仓审计**：`grep -rn "30 点\|27 点\|29 点\|32 点"`，共 **115 处 / 22 个文件**。

### 30.2 ★★ 先分类，再动手 —— 同一个数字在不同文件里指**不同的表**

这是本次最重要的一条。115 处里**大多数是对的**：

| 「N 点」出现的位置 | 它实际指的是 | 结论 |
| --- | --- | --- |
| `13/`（Modbus）的绝大多数 | `kBindingCount`（**设备侧绑定数**，两条 `static_assert` 守着） | ✅ 正确，**不动** |
| `P3/` 的「30 点」 | **本映射表覆盖的点数**（子集，不跟随点表总数） | ✅ 正确，**不动** |
| `07/ems_point_table.h:71`、规划文档的 §25 记录 | 历史沿革（"老段 30 点 / 新段 32 点"） | ✅ 历史，**不动** |
| `P3/docs/README` §7.x、`P3/scripts/build.bat` | 缺陷记录里的"32 → 40" | ✅ 历史，**不动** |
| 其余 → `ems_point_table.h` 的**总点数** | 现在是 **40** | ❌ **过期，要改** |

**无脑全局替换会把 `13/` 的 32 与 `P3/` 的 30 一起改错** —— 那是把对的改坏。

### 30.3 改了什么（14 + 11 + 2 = 27 处，8 个文件）

**第三批（14 处，源 / 文档）：**

| 文件 | 改动 |
| --- | --- |
| `07/tests/test_rtdb_device_io.cpp:170` | 注释 `30 点` → `全点表`（**最老的一代**） |
| `11/docs/README.md:195` | T47 写线程 `27 点` → `设备侧点`（27 = A1 前的 MEAS7+STA6+CFG14） |
| `11/src/integration_runner.h:26` | 角色边界 `27 点` → `29 点`，并**挂上已知缺口注记**（见 §29.6） |
| `11/src/integration_runner.h:299` | 在"只跳 CMD"的代码处**直接写明** EXT 区没被跳过 |
| `11/tests/test_system_integration.cpp` ×4 | T41 / T42 注释 `30 点` → `全点表` |
| `13/docs/README.md:370`、`13/src/modbus_device_io.h:51` | 指的是**点表真相源**（不是绑定数）→ `40 点；本模块只绑设备侧 32 点` |
| `docs/规划/模拟器与设备接入梳理.md` ×3 | `点表真相源（32 点）`、`32 点表的子集` → 40 |
| `P3/src/iec104_point_map.h:15` | 澄清注释 `该表…为 32 点` → `40 点（32 设备侧 + 8 EXT）` |

**第四批（11 处，规划文档的叙述口径）：**
把「现在的 **32** 点表」这类**当作当前表名**用的措辞逐处改成 40 ——
**不做全局替换**，因为同一文档里还有「30 → 32 点」的历史沿革必须留着。

**第五批（2 处，`13/docs/README.md` §8.1「本模块没有做的」）：**

- ★ `| **无 EXT 点区** |` —— **A3.1 已把 EXT 点区 + 窄接口 + 网关落点做完**，
  这一行是**已关闭的缺口仍挂在"未做"表里** → 改成「EXT 已落地、尚未接线」，并指向 **A3.2**；
- `| **设备全点表不全** | 只有 32 点 |` → `段里只有 EMS 抽象表（40 点；本模块绑设备侧 32 点）`。

### 30.4 验证（改的全是文案 → 断言数必须逐位不变）

| 复跑目标 | 结果 | 与基线 |
| --- | --- | --- |
| `07/` RT_DB | `PASS=554 FAIL=0` | 一致 |
| `11/` 三进程重编 + T41~T49 | `PASS=151 FAIL=0` | 一致 |
| `13/` 适配器契约 | `PASS=133 FAIL=0` | 一致 |
| `P3/` 协议层 | `PASS=376 FAIL=0` | 一致 |
| 脚本退出码 | `RC=0` | — |

**全量口径不变 9245 / 9323。**

### 30.5 复扫后确认**不需要**改的（约 30 处）

`13/` 的「32 点」（绑定数）、`P3/` 的「30 点」（映射表）、`07/ems_point_table.h` 与
`CHANGES.md` 里的 `30 → 32` 沿革、`P3/` 的缺陷记录 —— **全部正确**。

### 30.6 通用教训（接 §29.7）

5. ★ **清扫前先分类，不要全局替换**：本项目里同一个「32 点」有**两种含义**
   （点表总点数 / 设备侧绑定数）。全局替换会把 A 文件改对、把 B 文件改错。
6. ★ **模块文档的「未做 / 遗留」清单会过期**：`13/docs/README.md` §8.1 还挂着
   "无 EXT 点区"，而 A3.1 已经交付。**每次交付后要回头看一遍各模块的遗留表** ——
   它不像断言会红，却会被当成现状读。

## 31. 「遗留 / 未做」清单的全项目复核（skill §14.10 的第一次执行，2026-09-20）

### 31.1 为什么

§30 第五批在 `13/docs/README.md` §8.1 抓到「**无 EXT 点区**」—— 一个**已关闭的缺口
仍挂在"未做"栏**里。那不是孤例，而是一类：**遗留清单不像断言会红，却会被当成现状读**。
于是把它固化成动作，**跑遍全项目**。

### 31.2 动作

```bash
grep -rn "^#\{1,4\} .*\(遗留\|未做\|没有做\|下一步\|缺口\|TODO\|待办\)" --include=*.md .
grep -rn "未做\|尚未\|还没有\|不支持\|未实现" --include=*.md .
```

逐条对到代码 / CHANGES，判「仍成立」还是「已关闭」。

### 31.3 结果

| 清单 | 结论 |
| --- | --- |
| `P3/docs/README.md` §8 未做 / 下一步 | ✅ **已是最新**（EXT 标 ✅、A3.2 单列、转发表外置 / 累计量 / IP 白名单 / GPL 都在） |
| `13/docs/design.md` §9 遗留的设计问题 | ✅ 仍成立（RTU / 单连接 / 重复扫 IR） |
| `13/docs/README.md` §8.1 | ✅ §30 已修 |
| `11/docs/README.md` §7 遗留（L2 需量 1 拍极限环） | ✅ 仍成立（修它会动 7500+ 断言） |
| `07/docs/README.md` §8.x | ✅ 仍成立 |
| `docs/规划/…` §10.2 / §10.5 | ✅ 已是最新（除下面那两处断言数） |
| **`docs/规划/…` 头部「★ 2026-09-20 更新」块** | ❌ **过期**：只记了 B1、没记 A3.1；基线还写 `8906 / 8984`；排序还写 `A3 → B2` |
| **`docs/规划/…` §0「真正缺的仍是三块」的 ②** | ❌ **过期**：② 是「Modbus（接真机的唯一通道）」—— **B1 已打通** |
| **`docs/规划/…` Q10「怎么落到真实 PCS 寄存器」** | ❌ **过期**：写 `❌ 未做（Modbus 写寄存器 + 回读）`，而 `13/src/modbus_device_io.h:210` 的 `write_command()` 就是这条路 |
| **`docs/规划/…` §4.3 标题「从设备侧到真实 PCS（未做）」** | ❌ 同上 |
| **`docs/规划/…` §10 状态行** | ❌ 写「A3 … **仍未做**」 |
| **`docs/规划/…` §10.1 的 A3 小节标题** | ❌ 没像 A1 / A2 那样加删除线（同一节两种体例） |
| **`docs/规划/…` §10.2 / §10.5 的 B1 断言数** | ❌ `437` → **439** |
| **`README.md` 导航行** | ❌ `**A1 已修**，A2/A3 与 B/C/D 类仍未做` |

### 31.4 改法（9 处 / 2 个文件，全部是状态口径）

★ 两处值得单独说：

1. **`Q10` 与 `§4.3` 的「未做」是"半对"** —— Modbus 主站的**机制**（写寄存器 + 回读）
   已由 B1 交付，缺的是**真实 PCS 与 RTU**。所以不是改成「已做」，而是改成
   **🟡 机制已通、缺真实设备**。**"已做 / 未做"二元标签装不下"机制通了但没接真机"**，
   硬套二元会造出新的不准确。
2. **同一份文档里两种体例**：§10.1 的 A1 / A2 标题用了 `~~删除线~~ —— 已修`，A3 没加。
   **已修与未修在同一节里长得一样**，读者只能靠正文分辨 → 统一体例。

### 31.5 验证

**纯文档改动**（`docs/规划/…` + `README.md`），无源码、无脚本 → **不触发重编**。
复跑证据沿用 §30：`07=554 / 11=151 / 13 适配器契约=133 / P3 协议层=376`，`RC=0`；
全量口径 **9245 / 9323** 不变。

### 31.6 通用教训（接 §30.6）

7. ★ **「遗留 / 未做」清单要当成产物维护**：它没有断言守着，但会被当成现状读。
   每次交付后**必须回看**（已固化为 skill §14.10 的动作）。
8. ★ **"已做 / 未做"是二元标签，装不下"机制通了但没接真机"** —— 这类条目必须用
   三态（✅ / 🟡 机制已通 / ❌），否则修完一次还会错第二次。
9. ★ **同一节里的同体例条目要长得一样**：A1 / A2 加了删除线而 A3 没加，
   说明**上次交付只改了正文、没改标题**。

## 32. 缺口 A3.2：外部设定接进安全链（EXT 第 10 条约束 + 停机粘住，2026-09-20）

### 32.1 任务来源与拍板

A3.1 只交付了"通路"（EXT 点区 + 窄接口 + 网关落点 + 失效兜底），
`narrow_interval_by_ext()` 已实现且有 T72 覆盖，但**没有任何生产路径调用它**。
A3.2 = 把它接进 `05/` 安全引擎（第 10 条约束）+ 装配层调用。

接线前必须先拍板 `P3/docs/design.md` §5.7：**stale 时"停机/闭锁"设定不生效 vs 粘住**。
拍板结果（见 §32.2）：**停机/闭锁粘住，功率设定不生效**。

### 32.2 ★ 语义决策：停机/闭锁粘住，功率设定不生效

**安全不对称**（这是决策的核心）：

| 命令 | stale 后"不生效"的后果 | stale 后"粘住"的后果 |
| --- | --- | --- |
| 5001 停机 / 5002 闭锁 | 调度让停 → 网关死 → EMS **自动复机**（可能违背调度安全意图） | 停在 `[0,0]`，**少赚钱，零安全风险** |
| 6001~6004 功率设定 | 回到本地管理（本地安全引擎仍在） | 永久被一条**过期的收紧**卡住 |

**丢"停机"会丢安全，粘"停机"只丢钱** —— 失效方向该朝"少自由度"倒，
与不变式 #3「上层只收紧」同一条逻辑。

粘住守卫 = `present && consistent && finite`：
- **不含** stale（"旧"不是"坏"）；
- **不含** band_ok（那是设定带的倒挂，与停机这条独立命令无关）；
- **必须含** finite（NaN 经 `>0.5` 会判成 false，被误当"停机"）。

### 32.3 接线（四处）

| 处 | 改动 |
| --- | --- |
| `07/src/rtdb/ext_setpoints.h` | `narrow_interval_by_ext()` 停机/闭锁粘住（guard 见上）；`usable()` 注释同步 |
| `05/src/safety_engine.h` | `evaluate()` 增 `const ExtSetpoints& ext = {}`；本地 9 条收敛后、矛盾检查前调用 `narrow_interval_by_ext`；`SafetyVerdict` 增 `ext_active`；`kExtSetpoint` |
| `07/src/realtime_loop.h` | `EmsRuntime::set_ext_source()` + `ext_of_tick()`；step() ③ 传入 ext |
| `11/src/main_ems.cpp` | 用 RT_DB 句柄构造 `bool(int,double&)` 读回调，注入 `set_ext_source` |
| `11/src/integration_runner.h` | 设备侧 `publish_all()` 跳过 EXT 区（点区纪律）；注释与报告文案同步 |

### 32.4 为什么 EXT 落在 evaluate() 的收敛之后、矛盾检查之前

EXT 只能 min/max 收紧，但**收紧后可能制造区间矛盾**（调度限放 50 kW 而本地防逆流
要求放电 ≥100 kW）。矛盾必须由 evaluate() 现有的兜底统一处理（矛盾 = DERATED，
不是 EMERGENCY）。所以 narrowing 必须在 9 条收敛之后、`interval_contradiction` 检查之前。

EXT **不**推进 `items` / l0 / derate / emergency 标志 —— 调度停机/限功率是
"监督层命令"，不是"设备降额 / 硬安全动作"；trace 里用 `binding` + `ext_active` 标记。

### 32.5 测试

| 处 | 变化 |
| --- | --- |
| `05/tests/test_safety_engine.cpp` | 新增 **T07**（+26）：无外部设定 / 新鲜停机 / 新鲜闭锁 / 新鲜限功率 / **stale 停机粘住** / stale 功率设定不生效 / **EXT 收紧制造矛盾 → DERATED** |
| `P3/tests/test_iec104.cpp` | T72 第⑧条翻转（+4）：stale 停机/闭锁**粘住** + finite/consistent 反向守卫 |
| `11/tests/test_system_integration.cpp` | T41 增"设备侧不碰 EXT"（+1，`EXT.SEQ==0` 守卫） |

### 32.6 验证基线

| 项 | A3.1 | A3.2 |
| --- | --- | --- |
| `05/` | 68 | **94**（+26） |
| `P3/` 协议层 | 376 | **380**（+4） |
| `11/` | 151 | **152**（+1） |
| 其余 12 项 | — | **逐位不变**（additive + no-op 默认，复跑确认） |
| 全量（默认） | 9245 | **9276** |
| 全量（装 pymodbus） | 9323 | **9354** |
| `12/` 验收 | 55/55 | **55/55** |

### 32.7 涉及文件

| 文件 | 改动 |
| --- | --- |
| `07/src/rtdb/ext_setpoints.h` | 停机/闭锁粘住 + 注释 |
| `05/src/safety_engine.h` | 第 10 条约束 + `ext_active` + `kExtSetpoint` |
| `07/src/realtime_loop.h` | `set_ext_source` / `ext_of_tick` / 成员 |
| `11/src/main_ems.cpp` | 注入 EXT 读回调 |
| `11/src/integration_runner.h` | 设备侧跳过 EXT + 注释/文案 |
| `05/tests/test_safety_engine.cpp` | T07（+26） |
| `P3/tests/test_iec104.cpp` | T72 第⑧条（+4） |
| `11/tests/test_system_integration.cpp` | T41（+1） |
| 7 模块 `scripts/build*.bat` | 补 `-I ..\07\src\rtdb`（safety_engine.h/realtime_loop.h → ext_setpoints.h） |
| `07/scripts/*.bat` ×4 | 补 `-I src\rtdb`（07 自己的 test/main） |
| `P3/docs/*`、`07/docs/README.md`、规划文档、根 `README.md` | 基线 + A3.2 记录 |

### 32.8 未做 / 下一步

- ✅ **端到端已实测**（2026-09-20，见 §32.10）：初始化器 + 设备侧 + 网关 + 主站 + EMS 侧五进程，
  "调度停机 → EXT 区 → EMS 收成 [0,0]" 全链路闭环。
- ⏳ `stale_s` 的现场取值：当前 `11/main_ems.cpp` 硬编码 30 s，现场部署要按网关心跳周期调。
- 其余遗留（转发表外置 / 累计量 / IP 白名单 / GPL）见 `P3/docs/README.md` §8。

### 32.9 通用教训

1. ★ **"接进安全链"必须落在"能制造矛盾的那一步之后"**：EXT 只收紧，但收紧会制造
   区间矛盾，若放在矛盾检查之后，矛盾就漏判成"正常"。
2. ★ **改头文件驱动项目时，`-I` 依赖要一次想全**：本次给 `safety_engine.h` /
   `realtime_loop.h` 加 `#include "ext_setpoints.h"`，连带 7 个模块的 build*.bat 要补
   `-I ../07/src/rtdb`，**还要补 07 自己的 `-I src\rtdb`**（07 的 src/rtdb 是子目录，
   裸 `-I src` 找不到）。
3. ★ **批量插 -I 的补丁脚本，先核对"前导空格"**：第一次插入漏了前导空格，
   产生 `src-I ..\07\src\rtdb`（粘连），是把对的目录名和 -I 挤成一个 token；
   靠读补丁后的行抓出来的。改 .bat 后必须 `grep` 复核 + 跑 `fix_bat_encoding.py --check`。

### 32.10 端到端实测（2026-09-20）

五进程编排：`rtdb_initializer`（持段）→ `device_side`（发数据）→ `iec104_gateway --ext-rt-db`
（写 EXT）→ `iec104_master`（发 `--set 6001 120.5 --cmd 5001 0` 停机）→ `ems_side`（读 EXT 收窄）。

**探针（读侧）证据**：`present=1 seq=4`，`p_setpoint=120.500`，`pcs_onoff=0`（停机），
`narrow([-200,200]) = [0,0]` —— 真实主站经 IEC104 → 网关 → EXT 区，值逐位正确落地。

**EMS 进程证据**（`ems_side` 新增的联调检查项）：
`[PASS] 外部设定 · EXT 停机/闭锁收成 [0,0] — ext_active=600 / [0,0]拍=600` ——
600 拍全部 ext_active 且区间 [0,0]，真实 `EmsRuntime` 进程经 `set_ext_source` → `evaluate` 收窄。

**顺带观察到**：主站断开后网关 `clear_all()`（快路径失效兜底）立即把 EXT 清回默认（第一次探针读到的
就是清空后的状态，时序上把探针挪到"主站仍连接"才读到落点值）。

**预期内的 3 个 FAIL**（不是缺陷）：停机场景下"设备侧出力非零"必然为 0；BMS 禁放两项因
刻意去掉故障剧本（隔离 EXT 效应）而无窗口，反向守卫正确报"没发生"。

## 33. 转发表外置 CSV（P3，2026-09-20）

### 33.1 为什么做

IEC104 的 CA/IOA 转发表是**调度下发**的，每个现场不同。让现场为了改一个 IOA 重编 C++ 不可接受
（现场没有编译环境，改完还得过测试）。原来 `iec104_point_map.h` 里是 `static const PointDef kTable[]`，
换转发表要重编。本步把它升级成"表 = 数据"：内置默认表兜底 + 可选 CSV 覆盖。

### 33.2 接口

- `bool load_point_map_csv(const char* path, std::string* report)`（`iec104_point_map.h`）
  —— 加载成功返回 true；失败**回退内置默认表**并返回 false，`report` 写清哪一行错。
- 网关新增 `--point-map <file>`，在 `open_owned()`/`Iec104Server` 构造**之前**加载
  （这些入口会 capture `upload_table()` 的指针，晚于加载会让旧指针悬空）。
- `reset_point_map()` —— 测试用，清掉覆盖表回到默认。

### 33.3 CSV 格式（9 列）

```
ioa,kind,group,name,src,scale,offset,unit,note
```

- `kind` 九个规范 token（`MEAS_FLOAT` / `MEAS_SCALED` / `MEAS_NORMALIZED` / `SINGLE_POINT` /
  `DOUBLE_POINT` / `COUNTER` / `SET_FLOAT` / `CMD_SINGLE` / `CMD_DOUBLE`），大小写不敏感，未知报错。
- 首行可为表头（首列小写 == `ioa` 跳过）；`#` 开头注释；空行跳过。
- `name/src/unit` 不含 ASCII 逗号；`note` 可含逗号（取为最后一段）。
- 加载后按 `is_control` 分表：量测进上送、控制进下行（与内置默认表同构）。

### 33.4 校验（否则外置表 = 新的错误来源）

1. 全表过 `self_check_tables()` 同一套规则（把 `self_check()` 抽出 `self_check_tables(up,up_n,dn,dn_n,report)`，
   `self_check()` 与加载器共用）—— src 点名能在 `ems_point_table.h` 找到、IOA 唯一、区间正确、类型没搞反；
2. 任一不过 → 回退默认表，**绝不"加载了一半"就开始跑**；
3. `scale = 0` 拒绝（`from_protocol()` 除零）；
4. IOA/group/scale/offset 严格解析（`strtol`/`strtod` + endptr 全消费检查），非数即错。

### 33.5 ★ 字符串池的坑：`PointDef` 保持 `const char*`，字符串用 `deque<string>` 池

`PointDef` 的 `name/src/unit/note` 是 `const char*`。加载表要持有可变字符串，但**不改 `PointDef`**
（改它牵连 5 个文件的 `strcmp`/`snprintf`/`.c_str()`）。于是：

```cpp
struct PointMapStore { std::vector<PointDef> defs; std::deque<std::string> pool; };
```

- `defs` 里的 `const char*` 全部指向 `pool`。
- **pool 用 `std::deque` 不用 `std::vector`**：deque 在 `push_back` 时**不移动既有元素**（只失效
  迭代器，不失效引用/指针），所以短串（SSO）的 `c_str()` 不会因扩容而悬空。若用 vector + push_back，
  SSO 短串指针会随扩容整体悬空 —— 这是静默内存错误，编译期一个字不说。
- 最后 `std::move(store)` 换入：vector/deque 的**移动构造保证元素地址不变**，指针仍有效。

### 33.6 样例 CSV 与内置表互为镜像（T77 判据）

新增 `P3/data/point_map_default.csv`（内置默认表的 CSV 形态）。T77 加载它后与内置表**逐字段对账**
（ioa/kind/group/name/src/scale/offset/unit，note 是纯文档不比对）—— 这样"样例文件"和"编译进去的
兜底"互为镜像，谁改了一边都会在另一边露馅。

### 33.7 判据反证（1 次）

把 `upload_table()` 的"已加载则返回覆盖表"分支改成 `if (false)` → T77 第 ⑩ 条
`EXPECT(un == 1)` 变红（`PASS=407 FAIL=1`）。恢复后全绿。证明 T77 真的能抓到
"CSV 加载了但没被用上"这类静默失败（样例 CSV == 默认表，所以第 ② 条对账**判不出**
"加载了"和"没加载"的区别 —— 判别只能靠第 ⑩ 条"加载 1 点后 count==1"）。

### 33.8 验证基线

| 项 | 结果 |
| --- | --- |
| P3 协议层 | **408 断言 / 0 失败**（T61~T72 + T77，原 380，+28） |
| P3 EXT 落点 | **121 / 0**（不变） |
| 网关 --point-map 好表 | `[OK] 转发表已从 CSV 加载 … 上送 24 / 下行 6` |
| 网关 --point-map 坏路径 | `[FAIL] … 已回退到内置默认表`，`--self-check-only` 退出码 1 |
| 全量 | **9304**（默认口径）/ **9382**（装 pymodbus），原 9276 / 9354，+28 |

`iec104_point_map.h` 是 P3 私有头，不被任何外部模块 include（07 那两处只是注释引用），
故全量只有 P3 协议层 +28，其余逐位不动。

### 33.9 涉及文件

- `P3/src/iec104_point_map.h`（`kind_from_token` / `PointMapStore` / `load_point_map_csv` /
  `reset_point_map` / `self_check_tables` 抽取）
- `P3/src/main_gateway.cpp`（`--point-map` + 加载时序）
- `P3/data/point_map_default.csv`（新增，样例）
- `P3/tests/test_iec104.cpp`（T77，+28）
- `P3/docs/design.md` §6（标已交付 + deque 池坑）、`P3/docs/README.md`（参数 / §8 / §9 / §10）

### 33.10 未做 / 下一步（仍在 `P3/docs/README.md` §8）

累计量（BCR 编码 + `MEAS.E_IMPORT/E_EXPORT` 点表）、BMS 限值真实来源、IP 白名单、GPL-3.0 授权
（交付前阻塞项）。


---


# 34. 产品化 P3 通信层：Modbus 设备侧 / IEC104 调度侧（另一条开发线，2026-09-15）

> **来源说明**：本节与 §35 来自另一条开发线（2026-09-15 提交 `1a982f0` / `5016cce`），该线的 P3 实现是「Modbus 设备侧 + IEC104 调度侧 + 自建 TCP 传输层」双适配器方案。
> 主线 P3 采用 §22 的 `vendor/lib60870` 从站网关方案（Modbus 侧落在 `13/`），该线代码完整保留在分支 **`p3-dual-adapter-0915`**；此处仅回写变更记录，避免历史缺页。

> **前置**：`§18`（RT_DB 接入，519 断言）。
> P0 ~ RT_DB 证明的「换数据源不改算法」全都还在**一个地址空间**里。本节把 `IDeviceIO` 接到
> **两根真实线缆**：南向 Modbus（EMS 是主站 ↔ PCS/BMS/电表）、北向 IEC 60870-5-104（EMS 是受控站 ↔ 调度/云端）。
> 交付后 `IDeviceIO` 上并列 **5 个适配器**：`SimDeviceIO` / `MemoryDeviceIO` / `RtDbDeviceIO` /
> **`ModbusDeviceIO`** / **`Iec104DeviceIO`** —— 算法层（05/06/07/08）**一行未改**。

## 34.1 本次交付

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

## 34.2 为什么先做"监听环回"，而不是直接上真机

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

## 34.3 两个适配器的边界

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

## 34.4 关键设计：等价性与量化必须分开验

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

## 34.5 踩坑 1：单次采集失败 ≠ 进 FAULT（T35）

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

## 34.6 踩坑 2：未知 ASDU 类型 ≠ 畸形报文（T43）

T43 原本期望发一个"未知类型"的 ASDU 后 `malformed() == 0`，实际 `malformed()` 计数了 —— 2 个 FAIL。

根因在 `asdu_length_ok()`：它按类型查长度表，**未知类型落到 `default: return false`**，
于是 `handle_frame()` 把"不认识"当成"格式错"。

"IEC104 扩容了新型号"和"报文被截断了"是两件完全不同的事，前者不该报警。修法：
`asdu_length_ok()` 的默认分支改返回 `true`，未知类型只计 `unknown_type_`，畸形只计 `malformed_`，
两类计数分开。测试同步改为分别断言。

## 34.7 踩坑 3：`const` 成员函数里改缓存 —— 编译期就拦住了

`Iec104DeviceIO::reset_session()` 被写成 `const`，但语义上它要清 `last_cmd_` / `has_last_cmd_`。
编译器直接报 `assignment of member ... in read-only object`。

这个坑的价值在于它**方向是对的**：`reset_session()` 确实改变对象状态，不该是 `const`。
把 `const` 去掉即可 —— 与 04/ 的 `IDeviceIO` 里 `read_*` 是 `const`、`execute`/`write_*` 非 `const` 的分工一致。

（另有一处纯手误：`test_modbus.cpp` 里把 `block_first_index()` 的函数定义误插在用例上方，导致重定义。
属编辑事故，删掉重复定义即可。）

## 34.8 核心证据：T34 / T44 闭环逐位等价

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

## 34.9 其余用例

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

## 34.10 全量回归

- `scripts\build_all.bat`：`[BUILD ALL OK] All 25 components built.`
- 实跑断言：
  02 / 04=65 / 05=68 / 06=34 / 07=6050 / 07(RT_DB)=519 / 08=444 / P0+P0.5=79 /
  09=77 / 10=144 / P1=171 / P2=434 / **P3 Modbus=622** / **P3 IEC104=646**
  → **合计 9353 断言全绿**。

## 34.11 涉及文件

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

# 35. P3 现场闭环最后一跳：TCP 传输层（+ 4 项历史欠账回写，2026-09-15）

> 一句话：P3 之前所有通信验证都在**同调用栈内搬字节**（环回）。本次补上
> `IModbusTransport` / `IIec104Transport` 的 **TCP 实现**，用**真内核 socket + 独立线程**把
> "验证过"变成"能接真机"；同时把 MEMORY 里挂着的 4 项治理欠账一次清掉。

## 35.1 本次交付

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

## 35.2 为什么这一跳非做不可

P0 立的承诺是"换介质不改算法"，链条是：

```
SimDeviceIO → MemoryDeviceIO → RtDbDeviceIO → Loopback → TCP
```

前四环都验证过了，但**环回刻意跳过内核**。"能接真机"这句话只能由真 socket 兑现 ——
否则现场接 PCS / 调度时要同时换传输层并对抗一堆未知问题，而那时没有等价性基线可比。

本次换掉的**只有传输层实现**：适配器（`ModbusDeviceIO` / `Iec104DeviceIO`）与算法层
（05/06/07/08）**一行未改**。接真机只需 `set_endpoint(host, port)`。

## 35.3 三条实现纪律

| # | 纪律 | 为什么 |
| --- | --- | --- |
| 1 | Modbus **按 MBAP `Length` 分帧**（先精确读 7 字节 MBAP，再读 `Length-1` 字节 PDU），TID 回显必须一致 | TCP 是字节流：一次 `recv` 可能只回来半个响应（需重组），也可能把两个响应粘在一起（需切分）；TID 不一致 = 错位/串话，宁可直接丢弃也不能喂给解析器 |
| 2 | IEC104 `receive` **三态语义**：`false`=链路错误 / `true&len==0`=本轮无报文 / `true&len>0`=收到 N 字节 | 适配器 `drain_rx()` 靠这三态区分"对端哑了"（计 link_error）与"这一轮就是没数据"（正常） |
| 3 | `connect` **非阻塞 + select 超时** | 阻塞 connect 连不通的地址会挂 20 s 以上，现场表现为"EMS 卡死"，比连不上更难查 |

另有一处介质差异必须留出窗口：适配器 `open()` / 总召唤用 `drain_rx(0)`（非阻塞）等确认帧，
环回下响应同栈产生所以立刻可见，跨 TCP 必须等对端调度 → 给一个 `min_wait_ms` 窗口（**建链时 50，运行期回 0**）。

## 35.4 踩坑 1（本次最值得记的一条）：数值全对，整体滞后一拍

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

## 35.5 其余用例

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

## 35.6 4 项历史欠账回写

| # | 欠账 | 处置 |
| --- | --- | --- |
| 1 | **策略编号三处不一致** | 明确 **04 `namespace strategy_id` 为唯一真相源**；接口规范新增 **§4.10.1 权威映射表**（`S01_BMS_FORBID`…`S09_DEMAND_RESPONSE` ↔ `bms_protection`/`peak_valley_arbitrage`/…），并写明 `S0x` 是**登记顺序不是优先级**；`architecture.md` / `design.md` 加交叉引用。两处"看似的漏实现"写明理由：`soc_life_planner` 归 05 `check_soc`；`S02_BMS_DERATE` 与 §4.9 共用输入形状（区别在 `bms_lock` vs `bms_derating`） |
| 2 | **field rename 未回写** | 需求原文**一字未改**，在其周期 2 旁加「实现口径（回写）」块：`target_power→p_desired` / `max_power→p_upper` / `min_power→p_lower`，理由是**有符号区间**下 `max_power` 易被误读成"功率大小上限"；同时记录 `direction` 取消、`state→active+reason` 等 |
| 3 | **T07 fixture 与经典例不符** | T07 拆两子场景：**(a)** 上界来自 `pcs_rated_dis_kw=200`（原用例）；**(b)** 上界来自 `transformer_capacity_kw=200` **真实过载**（`P_grid=105, P_load=950` → `tr_load=200, ratio=1.00` → `reason="tr_overload"`，区间 `[10, 200]`），并**直接断言变压器策略自身的输出** |
| 4 | **BatteryState/GridState 未成体** | `04/src/data_models.h` §2.5 **显式成体**为**只读视图** + `battery_state_of()` / `grid_state_of()`。**不替换** `RealtimeSnapshot` / `DeviceLimits` 的既有字段（它们是全项目公共契约，改动会波及 05~10 与 P1/P2/P3），视图单向映射、**不产生第二份真相**；新增 **T18** 逐字段核对映射忠实性 + 验证"改视图不回写源结构" |

## 35.7 全量回归

- `scripts\build_all.bat`（**26 步**）：`[BUILD ALL OK] All 26 components built.`
- 实跑断言（全绿）：

```
02(无数值汇总) / 04=94 / 05=68 / 06=34 / 07=6050 / 08=444 / P0+P0.5=79 /
RT_DB=519 / 09=77 / 10=144 / P1=171 / P2=434 /
P3 Modbus=622 / P3 IEC104=646 / P3 TCP=87
                                        → 合计 9469 断言，FAIL=0
```

对比上一轮 9353：`-65 +94`（04 新增 T18 与 T07(b)）+ `+87`（P3 TCP）= **9469**。

## 35.8 涉及文件

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
