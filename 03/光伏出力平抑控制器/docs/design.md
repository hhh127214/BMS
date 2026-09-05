# 光伏出力平抑控制器详细设计文档（修正版）

| 文档编号 | EMS-L2-SMOOTH-001 |
| :--- | :--- |
| **版本** | V1.1（修正版） |
| **状态** | 已修订 |
| **编写** | B组 |
| **日期** | 2026-08-24 |
| **审核** | 待定 |
| **批准** | 待定 |

---

## 1. 引言

### 1.1 目的

本文档详细描述了工商业储能EMS中**光伏出力平抑控制器**的设计方案，包括控制目标、算法原理、软件结构、接口定义、参数配置、异常处理及测试方法。本文档是开发人员进行编码、测试和集成的基础依据。

### 1.2 范围

适用于储能EMS L2层实时闭环控制器中的光伏出力平抑模块。该模块以100ms控制周期运行，通过控制储能充放电来平滑光伏出力的分钟级波动，减小并网功率变化率，满足电网并网规范。同时，该模块需与防逆流、需量管理等策略协同，接受统一策略仲裁。

### 1.3 术语定义

| 术语 | 定义 |
| :--- | :--- |
| 光伏出力平抑 | 利用储能系统的快速充放电能力，吸收或释放功率，抵消光伏出力的短时波动，使并网功率变化平缓。 |
| 一阶低通滤波 | 一种简单的信号平滑方法，通过加权当前值与上一拍输出值，滤除高频分量。 |
| 滤波时间常数 | 决定滤波强度的参数，时间常数越大，输出越平滑，但滞后也越大。 |
| 功率变化率 | 单位时间内功率的变化量，通常用kW/s或kW/min表示。 |
| SOC | 电池荷电状态，百分比。 |

### 1.4 参考文档

- 《工商业储能EMS L2层动态调节策略模块详细设计方案》
- 《策略仲裁与协同控制模块详细设计方案》
- 《需量管理控制器详细设计文档》

---

## 2. 需求分析

### 2.1 功能需求

| 编号 | 需求描述 |
| :--- | :--- |
| FR-01 | 系统应能实时获取光伏出力 `P_pv` 和并网功率 `P_grid`。 |
| FR-02 | 系统应能根据设定的滤波算法，计算平滑后的期望并网功率，并据此计算储能补偿功率 `P_comp`。 |
| FR-03 | 系统应支持滤波时间常数 `tau` 的在线调整。 |
| FR-04 | 系统应支持SOC上下限保护，当SOC超出设定范围时，自动对**相应方向**的功率进行衰减，避免过充过放。 |
| FR-05 | 系统应能与防逆流、需量管理等策略协调，通过策略仲裁模块确定最终功率指令。 |
| FR-06 | 系统应支持安全约束（BMS禁止充放、最大功率限制）的强制覆盖。 |
| FR-07 | 系统应能检测光伏数据有效性，通信中断时安全闭锁。 |

### 2.2 性能需求

| 编号 | 指标 | 要求 |
| :--- | :--- | :--- |
| PR-01 | 控制周期 | 固定100ms（±5%） |
| PR-02 | 平滑效果 | 在典型光伏波动（如云层遮挡）下，并网功率1分钟变化率 ≤ 10%装机容量/分钟 |
| PR-03 | SOC保护响应 | 当SOC越限时，平抑输出应在1秒内开始衰减 |
| PR-04 | 与其他策略的协调 | 当高优先级策略激活时，平抑输出被覆盖，恢复后应平滑过渡 |

### 2.3 接口需求

| 接口 | 方向 | 数据 | 周期 |
| :--- | :--- | :--- | :--- |
| 光伏逆变器/电表 | 输入 | `P_pv` | 100ms |
| 关口电表 | 输入 | `P_grid` | 100ms |
| BMS | 输入 | `SOC`, `P_chg_max`, `P_dis_max`, 充放电使能 | 100ms/变化时 |
| 数据有效性标志 | 输入 | `data_valid` | 100ms |
| 策略合并器 | 输出 | `P_smooth_req`（补偿功率，正为放电，负为充电） | 100ms |
| 配置参数 | 输入 | `tau`, `SOC_low`, `SOC_high`, 等 | 变化时 |

---

## 3. 总体设计

### 3.1 模块定位

光伏出力平抑控制器与需量管理、防逆流控制器并列，同属L2层。其输出为补偿功率需求，需进入策略合并器与其它策略输出协调。由于平抑控制是持续动态过程，通常建议在需要平滑的时段激活，而在夜间或无光伏时关闭。

### 3.2 运行状态

- **正常平滑**：实时计算补偿功率。
- **SOC方向性衰减**：当SOC接近边界时，仅对需要限制的方向（充电或放电）按比例衰减输出，避免过充过放，同时不影响另一方向功率。
- **安全闭锁**：当BMS禁止充放电或通信故障时，输出置零。
- **待机**：平抑功能关闭（如夜间、用户手动关闭），输出为零。

---

## 4. 详细设计

### 4.1 控制算法：一阶低通滤波法

#### 4.1.1 基本原理

光伏出力平抑的目标是使并网功率 `P_grid` 平滑。由于 `P_grid = P_pv + P_bat`（此处简化，假设负载已并入P_grid或忽略负载），可以通过控制储能功率 `P_bat` 使得 `P_grid` 等于光伏功率经过低通滤波后的值 `P_smooth`。

一阶低通滤波的离散化公式为：

$$
P_{smooth}(k) = \alpha \cdot P_{smooth}(k-1) + (1 - \alpha) \cdot P_{pv}(k)
$$

其中：

- `P_smooth(k)`：当前拍平滑后的并网功率目标值。
- `P_smooth(k-1)`：上一拍平滑值。
- `P_pv(k)`：当前拍光伏实时功率。
- `α`：滤波系数，`α = τ / (τ + Ts)`，`τ` 为滤波时间常数，`Ts` 为控制周期。

则储能需要提供的补偿功率为：

$$
P_{comp}(k) = P_{smooth}(k) - P_{pv}(k)
$$

- 当 `P_comp < 0`：表示光伏突增，需要储能充电吸收功率。
- 当 `P_comp > 0`：表示光伏突降，需要储能放电支撑。
- 当 `P_comp = 0`：无需补偿。

#### 4.1.2 滤波时间常数的选择

- **τ 越大**：平滑效果越好，但储能需要承担更大的功率波动，且并网功率响应滞后越大。
- **τ 越小**：储能动作幅度小，但平滑效果差。

典型值：`τ = 60s`。可根据电网要求（例如1分钟波动限制）和储能容量调整。例如，若希望平滑1分钟以上的波动，可取 `τ = 60~120s`。

#### 4.1.3 离散化公式推导

控制周期 `Ts = 0.1s`，则：

$$
\alpha = \frac{\tau}{\tau + Ts}
$$

例如 `τ=60s`，`Ts=0.1s`，则 `α ≈ 0.9983`。

### 4.2 SOC方向性衰减策略

平抑控制会导致储能频繁充放电，可能使SOC持续偏离理想范围，甚至触及上下限。为避免过充过放，必须引入SOC管理。本设计采用**方向性衰减**：当SOC超出正常工作区时，仅对**需要限制的功率方向**进行衰减，另一方向不受影响。

#### 4.2.1 衰减系数计算

定义SOC正常工作区间 `[SOC_low, SOC_high]`（如20%~80%）。绝对安全上限 `SOC_max`（如90%），绝对安全下限 `SOC_min`（如10%）。

- **当 `SOC > SOC_high` 且补偿功率为充电（`P_comp < 0`）**：对充电功率进行线性衰减，衰减系数：
  $$
  decay = \max\left(0,\ \frac{SOC_{max} - SOC}{SOC_{max} - SOC_{high}}\right)
  $$
  放电功率（`P_comp > 0`）不受影响。

- **当 `SOC < SOC_low` 且补偿功率为放电（`P_comp > 0`）**：对放电功率进行线性衰减，衰减系数：
  $$
  decay = \max\left(0,\ \frac{SOC - SOC_{min}}{SOC_{low} - SOC_{min}}\right)
  $$
  充电功率（`P_comp < 0`）不受影响。

- **其他情况**：`decay = 1.0`，不改变原补偿功率。

#### 4.2.2 防止SOC漂移的补偿（可选）

为避免长时间平抑导致SOC单向漂移，可引入一个慢速的SOC回归项：当SOC偏离目标中心 `SOC_mid = (SOC_low+SOC_high)/2` 时，在补偿功率上叠加一个小的偏置，使SOC缓慢回归。但该偏置应远小于平抑功率，避免影响平滑效果。本版本暂不实现，留作扩展。

### 4.3 与其他策略的协调

平抑控制器输出的是**补偿功率需求**，并不直接下发。在策略合并器中，需与防逆流、需量管理等策略按优先级协调：

- **防逆流**优先级通常高于平抑：当逆流风险出现时，防逆流控制器要求充电，应覆盖平抑的放电需求（或充电需求），确保不逆流。
- **需量管理**优先级高于平抑：当需量接近限值时，需量控制要求放电削峰，应覆盖平抑的充放电需求。
- **安全约束**最高：BMS禁止充放电时，直接闭锁所有输出。

因此，平抑功能应在系统安全且无更高优先级调节需求时生效。实际合并时，平抑输出可视为“默认功率”，被其他策略覆盖时，平抑控制器应能平滑退出，避免功率突变。

### 4.4 软件接口定义

#### 4.4.1 输入数据结构

```cpp
struct SmoothControllerInput {
    double P_pv;          // 光伏实时功率 (kW)
    double SOC;           // 电池SOC (%)
    double P_chg_max;     // 最大允许充电功率 (kW)
    double P_dis_max;     // 最大允许放电功率 (kW)
    bool   charge_enabled;   // 是否允许充电
    bool   discharge_enabled;// 是否允许放电
    bool   smoothing_enabled; // 平抑功能是否启用
    bool   data_valid;        // 数据有效性（通信正常为true）
};
```

#### 4.4.2 输出数据结构

```cpp
struct SmoothControllerOutput {
    double P_smooth_req;  // 补偿功率需求 (kW)，正放电，负充电
    bool   active;        // 是否激活
    double decay_factor;  // 当前衰减系数（诊断用）
};
```

### 4.5 伪代码（修正版）

```python
class SmoothingController:
    def __init__(self, config):
        self.tau = config['tau']               # 滤波时间常数 (s)
        self.Ts = config['Ts']                 # 控制周期 (s)
        self.SOC_low = config['SOC_low']       # 正常工作区下限 (%)
        self.SOC_high = config['SOC_high']     # 正常工作区上限 (%)
        self.SOC_min = config['SOC_min']       # 绝对下限 (%)
        self.SOC_max = config['SOC_max']       # 绝对上限 (%)
        self.alpha = self.tau / (self.tau + self.Ts)
        self.P_smooth_prev = 0.0
        self.last_output = 0.0
        self.first_update = True

    def update(self, input):
        # 数据无效或功能关闭：输出0，重新初始化
        if not input.data_valid or not input.smoothing_enabled:
            self.P_smooth_prev = input.P_pv
            self.last_output = 0.0
            self.first_update = True
            return 0.0

        if self.first_update:
            self.P_smooth_prev = input.P_pv
            self.first_update = False
            self.last_output = 0.0
            return 0.0

        # 一阶低通滤波
        P_smooth = self.alpha * self.P_smooth_prev + (1 - self.alpha) * input.P_pv
        self.P_smooth_prev = P_smooth

        # 补偿功率（理想值）
        P_comp = P_smooth - input.P_pv  # 负值充电，正值放电

        # SOC方向性衰减
        decay = 1.0
        SOC = input.SOC
        if SOC > self.SOC_high and P_comp < 0:
            # 高SOC，只衰减充电
            decay = max(0.0, (self.SOC_max - SOC) / (self.SOC_max - self.SOC_high))
            P_comp *= decay
        elif SOC < self.SOC_low and P_comp > 0:
            # 低SOC，只衰减放电
            decay = max(0.0, (SOC - self.SOC_min) / (self.SOC_low - self.SOC_min))
            P_comp *= decay
        # 其他情况不衰减

        # 安全限幅与使能
        if P_comp < 0:  # 充电
            if not input.charge_enabled:
                P_comp = 0.0
            else:
                P_comp = max(-input.P_chg_max, P_comp)
        else:           # 放电
            if not input.discharge_enabled:
                P_comp = 0.0
            else:
                P_comp = min(input.P_dis_max, P_comp)

        self.last_output = P_comp
        return P_comp

    def reset(self):
        self.P_smooth_prev = 0.0
        self.last_output = 0.0
        self.first_update = True
```

### 4.6 C++参考实现（修正版）

#### 4.6.1 头文件 `SmoothingController.h`

```cpp
#ifndef SMOOTHING_CONTROLLER_H
#define SMOOTHING_CONTROLLER_H

#include <cmath>
#include <algorithm>

class SmoothingController {
public:
    struct Config {
        double tau;          ///< 滤波时间常数（秒）
        double Ts;           ///< 控制周期（秒）
        double SOC_low;      ///< 正常工作区下限（%）
        double SOC_high;     ///< 正常工作区上限（%）
        double SOC_min;      ///< 绝对下限（%）
        double SOC_max;      ///< 绝对上限（%）

        Config()
            : tau(60.0),
              Ts(0.1),
              SOC_low(20.0),
              SOC_high(80.0),
              SOC_min(10.0),
              SOC_max(90.0) {}
    };

    explicit SmoothingController(const Config& cfg);
    ~SmoothingController() = default;

    /**
     * @brief 更新平抑控制器
     * @param P_pv            光伏实时功率（kW）
     * @param SOC             电池SOC（%）
     * @param P_chg_max       最大允许充电功率（kW）
     * @param P_dis_max       最大允许放电功率（kW）
     * @param charge_enabled   是否允许充电
     * @param discharge_enabled 是否允许放电
     * @param smoothing_enabled 平抑功能是否启用
     * @param data_valid       光伏数据有效性（通信正常为true）
     * @return 补偿功率需求（kW），正为放电，负为充电
     */
    double update(double P_pv, double SOC,
                  double P_chg_max, double P_dis_max,
                  bool charge_enabled, bool discharge_enabled,
                  bool smoothing_enabled, bool data_valid);

    void reset();

    double getLastOutput() const { return last_output_; }
    double getAlpha() const { return alpha_; }

private:
    Config cfg_;
    double alpha_;
    double P_smooth_prev_;
    double last_output_;
    bool first_update_;

    double clamp(double val, double lo, double hi) const {
        return std::max(lo, std::min(val, hi));
    }
};

#endif // SMOOTHING_CONTROLLER_H
```

#### 4.6.2 源文件 `SmoothingController.cpp`

```cpp
#include "SmoothingController.h"

SmoothingController::SmoothingController(const Config& cfg)
    : cfg_(cfg),
      alpha_(cfg.tau / (cfg.tau + cfg.Ts)),
      P_smooth_prev_(0.0),
      last_output_(0.0),
      first_update_(true) {}

double SmoothingController::update(double P_pv, double SOC,
                                   double P_chg_max, double P_dis_max,
                                   bool charge_enabled, bool discharge_enabled,
                                   bool smoothing_enabled, bool data_valid) {
    // 数据无效或功能关闭：输出0，重新初始化
    if (!data_valid || !smoothing_enabled) {
        P_smooth_prev_ = P_pv;
        last_output_ = 0.0;
        first_update_ = true;
        return 0.0;
    }

    if (first_update_) {
        P_smooth_prev_ = P_pv;
        first_update_ = false;
        last_output_ = 0.0;
        return 0.0;
    }

    // 一阶低通滤波
    double P_smooth = alpha_ * P_smooth_prev_ + (1.0 - alpha_) * P_pv;
    P_smooth_prev_ = P_smooth;

    // 理想补偿功率
    double P_comp = P_smooth - P_pv;

    // SOC方向性衰减
    if (SOC > cfg_.SOC_high && P_comp < 0.0) {
        // 高SOC，只衰减充电
        double decay = std::max(0.0, (cfg_.SOC_max - SOC) / (cfg_.SOC_max - cfg_.SOC_high));
        P_comp *= decay;
    } else if (SOC < cfg_.SOC_low && P_comp > 0.0) {
        // 低SOC，只衰减放电
        double decay = std::max(0.0, (SOC - cfg_.SOC_min) / (cfg_.SOC_low - cfg_.SOC_min));
        P_comp *= decay;
    }
    // 其他情况不衰减

    // 安全限幅与使能
    if (P_comp < 0.0) {  // 充电
        if (!charge_enabled) {
            P_comp = 0.0;
        } else {
            P_comp = std::max(-P_chg_max, P_comp);
        }
    } else {  // 放电
        if (!discharge_enabled) {
            P_comp = 0.0;
        } else {
            P_comp = std::min(P_dis_max, P_comp);
        }
    }

    last_output_ = P_comp;
    return P_comp;
}

void SmoothingController::reset() {
    P_smooth_prev_ = 0.0;
    last_output_ = 0.0;
    first_update_ = true;
}
```

---

## 5. 异常处理与安全保护

| 异常类型 | 检测方法 | 处理措施 |
| :--- | :--- | :--- |
| 光伏通信中断 | `data_valid = false` | 输出置零，重新初始化，告警 |
| BMS禁止充/放电 | `charge_enabled`/`discharge_enabled` 为假 | 对应方向输出置零 |
| SOC达到绝对上限 | `SOC >= SOC_max` | 充电衰减为0，仅允许放电 |
| SOC达到绝对下限 | `SOC <= SOC_min` | 放电衰减为0，仅允许充电 |
| 平抑功能关闭 | `smoothing_enabled=false` | 输出置零，并重新初始化滤波值 |
| 与其他策略冲突 | 策略合并器检测到更高优先级策略激活 | 平抑输出被覆盖，应平滑退出 |

---

## 6. 参数配置与整定

### 6.1 参数列表

| 参数 | 单位 | 默认值 | 范围 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `tau` | s | 60 | 10~300 | 滤波时间常数，决定平滑强度 |
| `SOC_low` | % | 20 | 10~40 | 正常工作区下限 |
| `SOC_high` | % | 80 | 60~90 | 正常工作区上限 |
| `SOC_min` | % | 10 | 5~20 | 绝对下限，禁止放电 |
| `SOC_max` | % | 90 | 80~95 | 绝对上限，禁止充电 |
| `Ts` | s | 0.1 | 固定 | 控制周期 |

### 6.2 参数整定建议

- **`tau`**：根据电网并网要求（如1分钟波动≤10%装机容量）和储能容量设定。若要求平滑更长时间尺度的波动，可增大`tau`，但同时需考虑储能容量能否支撑更大的功率波动。
- **SOC工作区**：根据电池类型和寿命要求设定。磷酸铁锂电池通常允许20%~80%正常工作，10%~90%为安全边界。
- **与防逆流的配合**：若同时启用防逆流和平抑，建议将平抑的`tau`设得稍大，避免与防逆流的快速调节冲突。

---

## 7. 测试方案

### 7.1 仿真测试场景

| 场景 | 初始条件 | 动作 | 预期结果 |
| :--- | :--- | :--- | :--- |
| 光伏波动平滑 | 光伏以正弦波波动，幅值±50kW，周期10分钟 | 启用平抑 | 并网功率波动显著减小，1分钟变化率达标 |
| SOC上限衰减 | SOC=85%，光伏快速增加 | 平抑请求充电 | 充电功率逐渐衰减，SOC接近90%时充电基本停止；若此期间光伏突降需要放电，放电功率不受衰减影响 |
| SOC下限衰减 | SOC=15%，光伏快速下降 | 平抑请求放电 | 放电功率逐渐衰减，SOC接近10%时放电基本停止；若此期间光伏突增需要充电，充电功率不受衰减影响 |
| 安全闭锁 | BMS禁止充电 | 平抑请求充电 | 充电输出置零，告警；放电仍正常 |
| 功能切换 | 平抑开启→关闭→开启 | 切换 | 输出平滑过渡，无冲击 |
| 通信中断 | `data_valid=false` | 通信恢复后重新启用 | 中断期间输出0，恢复后重新初始化 |

### 7.2 硬件在环测试

- 连接真实PCS模拟器、光伏模拟器和关口电表模拟器。
- 模拟云层遮挡导致的光伏剧烈波动，验证平抑效果和SOC保护动作。
- 验证与防逆流、需量管理同时激活时的策略仲裁是否正确。

---

## 8. 部署与运行要求

- **运行平台**：与需量管理、防逆流控制器相同，部署在L2实时控制器中。
- **控制周期**：100ms，由高精度定时器驱动。
- **日志记录**：记录平抑输出、SOC、衰减系数等关键数据，便于事后分析。

---

## 9. 附录

### 9.1 版本历史

| 版本 | 日期 | 修改内容 | 作者 |
| :--- | :--- | :--- | :--- |
| V1.0 | 2026-08-24 | 初稿完成 | B组 |
| V1.1 | 2026-08-24 | 修正SOC方向性衰减逻辑；增加`data_valid`接口；统一伪代码与C++实现；补充`getAlpha()`实现 | B组 |

---
