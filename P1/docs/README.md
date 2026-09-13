# P1 · 配置化

> 产品化路线第 3 步（P0 架构分层 ✅ → P0.5 适配器可换性 ✅ → **P1 配置化** → P2 可观测性 → P3 通信 → RT_DB 接入）
>
> **目标：现场部署不改源码，只改配置文件。**

---

## 1. 为什么需要 P1

P0 把设备 I/O 抽象成 `IDeviceIO` 之后，算法已经不知道底层是仿真 / RT_DB / Modbus。
但**装配过程本身**仍然散落在每个调用点，而且带 3 处隐式顺序依赖 —— 都不会在编译期报错：

```cpp
EmsRuntime rt;
rt.config()             = cfg.loop;
rt.safety_params()      = cfg.safety;
rt.coordinator_config() = cfg.coord;
rt.fsm_config()         = cfg.fsm;
rt.configure_plant(cfg.plant);   // ← 陷阱① 内部 refresh_device_limits() 重置 dev_
rt.device_limits()      = cfg.limits;   // ← 必须在这之后
rt.apply_configs();              // ← 陷阱② 用 dev_.pcs_rated_* 算协同层参数
rt.shaper().set_deadband(cfg.loop.output_deadband_kw);  // ← 陷阱③ 整形器缓存了副本
```

| 陷阱 | 机制 | 不知道会怎样 |
|---|---|---|
| ① `configure_plant()` 重置 `dev_` | 内部 `refresh_device_limits()` → `io_->read_limits(dev_)`，而 `SimDeviceIO::read_limits()` 第一行是 `out = DeviceLimits{}` | 配置里的 `transformer_capacity_kw` / `d_target_kw` 被**静默复位成默认值 250** |
| ② `apply_configs()` 依赖 `dev_` | 用 `dev_.pcs_rated_chg_kw` 计算协同层设备参数 | 优化层按**旧设备**排 96 点计划，与实时层不一致 |
| ③ `OutputShaper` 缓存副本 | `init()` 里 `shaper_.set_deadband(cfg_.output_deadband_kw)` 存的是**值**，不是引用 | 改了 `LoopConfig.output_deadband_kw`，**死区行为不变** —— "配置生效了但没生效" |

这三条属于"知道的人不会错、不知道的人必错"的知识。P1 把它收进 **`apply_config()` 一个函数**，
调用方只需 `apply_config(rt, cfg)`。

---

## 2. 模块结构

```
P1/
├── src/
│   ├── json_lite.h      最小 JSON 解析/生成（零依赖，支持注释/尾逗号/裸键）
│   ├── ems_config.h     配置模型 + 字段绑定表（Binder）
│   ├── config_loader.h  load / save / validate / apply / capture   ← 核心
│   ├── config_doc.h     配置模板 / Markdown 文档 / Schema 生成
│   └── main.cpp         演示程序（ems_config.exe）
├── tests/
│   └── test_config.cpp  T201 ~ T218
├── data/
│   └── ems_config.sample.json   现场配置样例（带注释）
├── docs/
│   └── README.md        本文件
└── scripts/
    ├── build.bat / build_test.bat / run_demo.bat
```

分层职责：

| 函数 | 方向 | 说明 |
|---|---|---|
| `load_config_file()` | 文件 → `EmsConfig` | 语法 + 类型检查，报错带 **行:列** |
| `validate()` | `EmsConfig` → 诊断 | **纯函数**，不碰运行时；只判"一定会跑坏"的为 error |
| `apply_config()` | `EmsConfig` → `EmsRuntime` | **唯一**拥有装配顺序的地方 |
| `capture_config()` | `EmsRuntime` → `EmsConfig` | 导出运行中的现场参数（归档 / 回滚） |
| `save_config_file()` | `EmsConfig` → 文件 | 与 load 共用同一张字段表 |

---

## 3. 配置模型

```jsonc
{
  "name": "...", "description": "...", "site": "...", "version": "1.0",
  "loop":        { /* 实时循环 */ },
  "plant":       { /* 被控对象 */ },
  "limits":      { /* 设备限制 */ },
  "safety":      { /* 安全约束 */ },
  "coordinator": { /* 优化调度与实时协同 */ },
  "state_machine": { /* 状态机 */ },
  "strategies":  { "S04_DEMAND_MGMT": { "enabled": true, "note": "...", "params": {...} } }
}
```

共 **89 个字段**（6 个 section）+ 策略条目。

**缺省语义**：没出现的字段一律沿用各层结构体的默认值。
一份现场配置只描述"这个站和默认有什么不同"。

- 缺整个 section → 该层全用默认值，**不逐字段提示**（这是刻意的，否则会刷屏）
- 配了某 section 但漏了标 ★ 的关键项 → **警告**（用默认值继续跑）
- 键名打错 → **警告**，指出未知键
- 类型不符 → **错误**，指出期待类型与实际类型

---

## 4. 字段绑定表（为什么不做手写 to_json / from_json）

手写两份映射（读一份、写一份）**必然漂移**：加了字段忘了写其中一份，
表现是"配置里改了但没生效"或"导出的配置少了字段"，且**编译期不报错**。

本模块用一张绑定表同时驱动 load 与 save：

```cpp
inline void bind_fields(Binder& b, SafetyParams& c) {
    b.dbl("soc_min", &c.soc_min, "1", "SOC 绝对下限（触及禁放）");
    ...
    b.require("soc_min");       // ★ 关键项
}
```

- 字段表与结构体**编译期绑定**（取地址），结构体改名即编译失败
- `--dump-template` / `--dump-doc` / `--dump-schema` 也从同一张表渲染 → **文档不会漂移**
- `--roundtrip` 用同一张表验证"读一遍写一遍"零漂移（T208 断言字段数 == 89，防止漏绑）

策略参数**不做字段级绑定**：`ParamMap` 是 `string → double`，
参数名由**各策略自己解释**（`Kp` / `deadband` / `margin_kw` …），P1 不可能也没必要知道。
P1 只负责按 id 精确定位（**id 打错必须报错**，静默忽略会让人以为"配了但没生效"）。
实测有效：本模块自带的样例配置第一次就写错了 id，被 `--check` 当场拦下。

---

## 5. 命令行

```bash
ems-config --demo                      # 内置场景 A/B 对照（默认）
ems-config --apply <file>              # 加载 → 校验 → 装配 → 跑 24h
ems-config --check <file>              # 只做语法 + 语义 + 装配检查
ems-config --roundtrip <file>          # 存取往返一致性
ems-config --dump-template             # 带注释的配置模板（可直接改完就用）
ems-config --dump-doc                  # 配置说明（Markdown）
ems-config --dump-schema               # 字段清单（机器可读 JSON）
ems-config --capture <out.json>        # 导出运行中的当前配置
ems-config --save-default <out.json>   # 导出内置默认配置
# 选项：--hours N（默认 24）--fast（dt=2s）--quiet
```

退出码：`0` 正常 / `1` 配置错误 / `2` 行为验证失败。

### A/B 对照演示（`--demo`）

证明"只改配置、不改源码"就能改变行为。只改 3 个参数：

| 参数 | 基线 | 收紧 |
|---|---|---|
| `limits.transformer_capacity_kw` | 630 | 200 |
| `limits.d_target_kw` | 320 | 200 |
| `safety.grid_p_max_kw` | 630 | 200 |

24 h 实测（`dt=1 s`，86400 拍）：

| 指标 | 基线(630) | 收紧(200) | 变化 |
|---|---|---|---|
| 关口峰值 | 434.0 kW | 292.1 kW | **−141.9 kW** |
| 购电量 | 4555.8 kWh | 4177.2 kWh | −378.6 kWh |
| 储能充电量 | 538.6 kWh | 3.2 kWh | −535.4 kWh |
| 储能放电量 | 365.8 kWh | 192.9 kWh | −172.9 kWh |
| SOC 区间 | 0.12 ~ 0.88 | 0.10 ~ 0.50 | 可用空间被压死 |
| 指令逃逸 | 0 拍 | 0 拍 | 硬不变量保持 |

收紧后储能几乎充不进电（变压器 200 kVA 不够同时带负荷和充电），
SOC 打到下限后只能靠 192.9 kWh 存量放电削峰 —— 这正是"配置过紧"的现场后果，
配置化让它在**离线阶段**就暴露出来，而不是并网之后。

---

## 6. 校验规则

`validate()` 是纯函数，只看配置本身。原则：**只把"一定会跑坏"的判成 error**，
"可疑 / 不合常理"判成 warning —— 现场调试时一个 warning 不该拦住启动。

| 类别 | 例子 |
|---|---|
| **error** | `dt_s ≤ 0`、`soc_min ≥ soc_max`、`soc_min < plant.soc_phys_min`（安全层失效）、`eta ∉ (0,1]`、`soc_init ∉ [soc_phys_min, soc_phys_max]`、`grid_p_min > grid_p_max`、`transformer_capacity_kw ≤ 0`、`log_every < 1` |
| **warning** | `enable_safety_engine = false`、`enable_state_machine = false`、`allow_ready_output = true`、`d_target > transformer_capacity`、`total_correction > l2_correction`、`grid_p_min > 0`（强制买电）、`soc_init` 落在禁充/禁放区 |
| **装配期 error** | 策略 id 不存在、策略参数被拒 |

`--check` 会额外做一次装配检查（建一个临时运行时，逐条应用策略），
这样"id 打错"能在**上电之前**发现。

---

## 7. 测试清单

`P1/tests/test_config.cpp` —— 171 条断言，全绿。

| 编号 | 内容 |
|---|---|
| T201 | JSON 解析：标量 / 对象 / 数组 / 嵌套 / 科学计数 |
| T202 | JSON 放宽语法：`//` `#` `/* */` 注释、尾逗号、裸键、裸标识符 |
| T203 | JSON 错误定位：报错带 行:列（缺冒号 / 未闭合 / 多余内容） |
| T204 | JSON 往返：`dump → parse` 幂等；转义正确；整数不带小数点 |
| T205 | 字段绑定：类型不符 → error；未知键 → warning；出错字段不污染 |
| T206 | 类型容忍：`"0.25"` → 0.25；`"on"`/`"false"` → bool；`1`/`0` → bool |
| T207 | 缺省语义：缺 section / 缺字段 → 保留结构体默认值 |
| T208 | 全配置往返幂等；**字段数 == 89**（漏绑护栏） |
| T209 | 语义校验：21 类 error + 3 类 warning 逐条命中 |
| T210 | 语义校验：默认配置 / 样例配置零错误 |
| **T211** | **装配顺序陷阱①**：反例（先 limits 再 configure_plant → 被冲掉）+ 正例 |
| **T212** | **装配顺序陷阱③**：`shaper().deadband()` 跟随配置；行为验证（5 kW 被 7.5 kW 死区清零） |
| **T213** | **装配顺序陷阱②**：协同层设备参数 == `dev_` |
| T214 | 策略装配：未知 id → error；`enabled` 与参数生效；`__weight__` 内部键被忽略 |
| T215 | 导出回灌：`capture → save → load → apply` 行为一致 |
| T216 | 端到端：改配置 → 关口峰值下降；硬不变量保持 |
| T217 | 关键项缺省提示；整节缺省不逐字段提示 |
| T218 | 文档生成：模板可被解析回读；Markdown / Schema 非空；Schema 字段数 == 89 |

T211 是最有说服力的一条 —— 它**同时验证了反例和正例**：

```cpp
// 反例：错误顺序 → limits 被 configure_plant 冲掉
bad.device_limits() = cfg.limits;
bad.configure_plant(cfg.plant);
EXPECT(bad.device_limits().transformer_capacity_kw != cfg.limits.transformer_capacity_kw);

// 正例：apply_config() 内部顺序正确
apply_config(good, cfg);
EXPECT_NEAR(good.device_limits().transformer_capacity_kw, cfg.limits.transformer_capacity_kw, 1e-9);
```

---

## 8. 现场部署怎么用

```bash
# 1. 生成模板
ems-config --dump-template > site.json

# 2. 按站点填参数（模板带注释，改完即可用）

# 3. 上电前校验（不跑仿真）
ems-config --check site.json

# 4. 离线验证 24h（确认硬不变量全通过）
ems-config --apply site.json

# 5. 现场运行一段时间后，把"实际生效的参数"导出归档
ems-config --capture tuned-20260912.json
```

第 5 步是现场调参的关键：调完之后 `--capture` 落盘的就是**真实生效值**，
可以直接作为回滚点或交接文档。

> **策略参数说明**：`--capture` 只导出**被显式设置过**的键。
> 未设置的参数走策略内置默认值（在策略代码里），不出现在导出结果中 —— 这是刻意的，
> 避免把默认值固化进配置，导致后续升级策略默认值时不生效。

---

## 9. 与其它步骤的关系

```
P0   架构分层（IDeviceIO）        ✅
P0.5 点表适配器验证               ✅
周期9  多策略组合测试             ✅
周期10 EMS 24h 离线仿真           ✅
 ↓
P1   配置化                       ← 本模块
 ↓
P2   可观测性（结构化日志 SOE / 指标导出 / 跟踪等级）
P3   通信（Modbus + IEC104）
RT_DB 接入（RtDbDeviceIO 适配器）
```

**现场装配的完整形态**（P1 + P3 就位后）：

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

注意 `inject_plant = false` —— 现场装配**不把配置里的 plant / limits 写进 `dev_`**，
因为真实设备的额定与限制是设备每拍给出的权威值，配置文件里的是参考值。
仿真装配（`10/`）才用 `inject_plant = true`。

---

## 10. 编译

```bash
cd P1
scripts\build.bat        # 编译演示程序
scripts\build_test.bat   # 编译 + 运行单元测试（171 条断言）
scripts\run_demo.bat     # 完整演示（6 步，产物在 build/）
```

依赖：`-I src -I ../04/src -I ../05/src -I ../06/src -I ../07/src -I ../08/src`。
零第三方依赖（JSON 解析器自带）。
