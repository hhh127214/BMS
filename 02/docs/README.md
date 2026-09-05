# 02/ — A 组（安全与保护）约束管理模块

> 100 ms 实时闭环的安全约束管理器，作为上层策略层（`01/`）与下层实时控制器（`03/`）之间的"安全闸门"。
>
> **本目录把上层 3 条安全策略落成可编译运行的 C++17 单文件骨架**，并演示 5 段串联场景下的合并结果。

---

## 1. 功能

把"上层策略 → 可执行功率"的链路压扁成一张"安全约束表"：

- 三个独立处理器，每个看一种传感数据：
  - **策略一 · BMS 禁止充放** — 读 BMS 的 `charge_enable / discharge_enable`，并监测通信超时 → 输出 `charge_blocked / discharge_blocked`
  - **策略二 · BMS 请求降功率** — 读 BMS 的 `max_charge_power / max_discharge_power`，加一阶低通去抖 → 输出 `charge_power_limit / discharge_power_limit`
  - **策略三 · 变压器过载限功率** — 读关口变压器的实时负载，按阈值/迟滞（滞环）算允许放电功率，并在极端过载（>110%）时强制禁放
- **合并器**：禁止标志取"或"（红灯盖过黄灯）；功率上限取 `min`；禁充/禁放时把对应功率上限压成 `0`
- 通信超时保护：BMS 数据陈旧（> 2 s） → 保守冻结，禁止充放

---

## 2. 文件清单

| 文件 | 说明 |
| --- | --- |
| `safety_constraint_manager.cpp` | 三个策略类 + 合并器 + 数据源接口 + Mock + 主循环入口（≈ 380 行，单文件可编译） |
| `safety_constraint_manager.exe` | g++ 编译产物（已包含） |
| `gen_figs.py` / `generate_ppt.py` / `gen_ppt.py` | 仿真图表、PPT 生成脚本（matplotlib + python-pptx） |
| `out.txt` | 演示运行输出（**UTF-16 LE + BOM**，记事本 / VSCode 打开可见） |
| `build.bat` | 一键编译：`g++ -std=c++17 safety_constraint_manager.cpp -o safety_constraint_manager.exe` |
| `run_demo.bat` | 运行演示并把 stdout 重定向到 `out.txt` |
| `safety_test.cpp` | 单元测试（assert 风格，单文件可编译） |
| `README.md` | 本文件 |

---

## 3. 编译

### 3.1 Windows（与已发布 .exe 同源）

```bat
cd 02
build.bat
```

或手动：

```bat
g++ -std=c++17 -O2 -Wall safety_constraint_manager.cpp -o safety_constraint_manager.exe
```

### 3.2 Linux（裸编译，无 Winsock 依赖）

```bash
cd 02
g++ -std=c++17 -O2 -Wall safety_constraint_manager.cpp -o safety_constraint_manager
./safety_constraint_manager | tee out.txt
```

> 仓库里的 `out.txt` 是 Windows 版本运行产物的 UTF-16 LE 编码日志（PowerShell `Out-File -Encoding Unicode` 或类似行为产生）。
> Linux 跑出来是 UTF-8，二者内容一致，仅编码不同。

---

## 4. 运行演示

```bat
run_demo.bat
```

控制周期 100 ms，总时长 9 s，覆盖 5 个串联场景：

| 时间段 | 场景 | 触发 |
| --- | --- | --- |
| 0 – 2 s | 正常 | 基线 |
| 2 – 4 s | BMS 请求降功率 | BMS 上送 `max_*_power` 下降 |
| 4 – 6 s | 变压器过载 | 关口负载由 180 kW 跳到 220 kW（负载率 ≈ 97.8%） |
| 6 – 7 s | BMS 禁止充电 | BMS `charge_enable=false` |
| 7 – 9 s | **BMS 通信中断** | 数据时间戳冻结在 6900 ms，age 持续增长，2 s 后触发超时保护 |

每 1000 ms 打印一次：
- 三个策略各自的中间结果
- 合并后的全局安全约束
- SOE（事件顺序记录）条目

---

## 5. 接口

`02/` 是上层策略与下层控制器的"中间层"。下游消费者只关心合并后的结果：

```cpp
struct SafetyConstraints {
    bool  charge_blocked;            // 是否禁止充电
    bool  discharge_blocked;         // 是否禁止放电
    float charge_power_limit_kw;     // 充电功率上限（kW，>0）
    float discharge_power_limit_kw;  // 放电功率上限（kW，>0；inf 表示不限）
    int64_t timestamp_ms;            // 本拍时间戳（用于数据时效追踪）
};
```

上层仲裁器（`03/三控制器集成/IntegrationMain.cpp`）按以下顺序使用：

```
1. 读取 SafetyConstraints
2. 若 charge_blocked    → 充电请求置 0
3. 若 discharge_blocked → 放电请求置 0
4. 充/放电功率请求再 clamp 到 charge_power_limit_kw / discharge_power_limit_kw
```

---

## 6. Mock 数据源

`safety_constraint_manager.cpp` 自带演示用 Mock：

- `MockBmsProvider` — 按仿真时间返回不同 BMS 状态（场景 1–5）
- `MockGridMeterProvider` — 返回关口变压器负载（场景 3 时跳变到 220 kW）

实际部署时把 `IBmsDataProvider` / `IGridMeterProvider` 替换为真实驱动即可，**主循环与策略代码完全不动**。

---

## 7. 单元测试

```bat
g++ -std=c++17 safety_test.cpp -o safety_test.exe
safety_test.exe
```

`safety_test.cpp` 用 `assert` 直接验证：

| 用例 | 验证点 |
| --- | --- |
| 1. 基线 | 所有上限=默认，禁止=false |
| 2. BMS 降功率 | 上限被低通收敛到 BMS 报告值 |
| 3. 变压器轻度过载 | 触发限功率，`discharge_limit` 下降 |
| 4. 变压器极端过载 (>110%) | `discharge_blocked` 被强制置 true |
| 5. BMS 禁止充电 | `charge_blocked=true` 且 `charge_power_limit=0`，放电不受影响 |
| 6. BMS 通信超时 | 通信 `age > 2s` 时禁止充放 |
| 7. 合并一致 | 禁止时功率上限=0，与标志位一致 |

退出码 `0` 即全部通过。

---

## 8. 与 B 组（`01/`）的关系

```
                ┌──────────────────────┐
                │  B 组策略（01/）      │      给出目标充放电功率
                │  MILP 三策略          │  ──────────────────────────┐
                └──────────────────────┘                             │
                                                                    ▼
                                                    ┌──────────────────────────┐
                                                    │  A 组安全约束（02/）       │
                                                    │  · BMS 禁止充放            │
                                                    │  · BMS 降功率              │
                                                    │  · 变压器过载限功率         │
                                                    └────────────┬─────────────┘
                                                                 │ SafetyConstraints
                                                                 ▼
                                                    ┌──────────────────────────┐
                                                    │  下层实时控制器（03/）     │
                                                    └──────────────────────────┘
```

A 组负责"绿灯/黄灯/红灯"，B 组负责"该跑多快"。B 组给出的功率计划被 A 组约束门压扁后，才交给 03/ 实时控制器执行。

---

## 9. 已知信息

- `out.txt` 是 UTF-16 LE + BOM 编码日志：记事本/VSCode 可读，命令行可能乱码；若需要纯 UTF-8 版，在 PowerShell 下运行 `Get-Content -Encoding UTF8` 重新保存即可。
- `gen_figs.py` / `generate_ppt.py` / `gen_ppt.py` 依赖 `matplotlib` / `python-pptx`：可用 `pip install matplotlib python-pptx numpy` 一次性安装。
- 单文件实现方便移植：策略类与主循环已用 namespace 隔离，整段复制到新工程即可启用。
