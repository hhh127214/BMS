# Battery Optimizer API — 接口文档

纯 C 实现（Winsock2 HTTP + 自研 MILP 求解器：单纯形 + 分支定界）的分时充放电优化服务。
**C 策略**包含三种：峰谷套利、动态预测优化、需求响应。
A 组 / B 组把电池参数、SOC 约束、电价预测（及负荷/光伏预测）以 JSON POST 给本服务，
服务返回每个时段的充放电功率计划。

---

## 1. 运行

```bash
build.bat            # 编译（gcc main.c json_util.c solver.c lp.c milp.c -lws2_32）
run.bat              # 启动，默认监听 0.0.0.0:8000；可带端口参数 run.bat 9000
```

调用地址：`http://<你的IP>:8000`
（Windows 防火墙需放行端口；本机测试可用 127.0.0.1）

---

## 2. 三种策略

| `strategy` | 策略 | 优化目标 |
| --- | --- | --- |
| `arbitrage` | 峰谷套利 | 低电价充电、高电价放电，**最大化套利收益** |
| `forecast` | 动态预测优化 | 给定负荷/光伏预测，**最小化购电成本**（每次调用都用最新预测重新优化 = 滚动时域优化） |
| `demand_response` | 需求响应 | 削峰，**最小化需量电费 + 购电成本**，可响应合同需量上限与 DR 信号 |

共同约束：SOC 动态方程、SOC ∈ [soc.min, soc.max]、充/放电功率上限、
不允许同时充放电（0-1 变量）、结束 SOC = soc.final。

---

## 3. 请求 JSON

`POST http://<你的IP>:8000/api/v1/optimize`，请求头 `Content-Type: application/json`

### 3.1 通用字段

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `strategy` | string | 必填：`arbitrage` / `forecast` / `demand_response` |
| `soc.initial` | number | 初始 SOC（0~1） |
| `soc.min` | number | SOC 下限（0~1） |
| `soc.max` | number | SOC 上限（0~1） |
| `soc.final` | number(可选) | 结束 SOC，缺省 = initial |
| `battery.capacity_kwh` | number | 电池容量（kWh） |
| `battery.max_charge_kw` | number | 最大充电功率（kW） |
| `battery.max_discharge_kw` | number | 最大放电功率（kW） |
| `battery.charge_efficiency` | number | 充电效率 (0,1] |
| `battery.discharge_efficiency` | number | 放电效率 (0,1] |
| `price` | array[number] | 电价预测（元/kWh），长度 = 时段数 N |
| `time_step_hours` | number(可选) | 每时段时长（小时），缺省 1.0 |

### 3.2 策略专属字段

**forecast / demand_response** 额外要求：
- `load_kw`（必填）：负荷预测，长度 = N（单位 kW）
- `pv_kw`（可选）：光伏预测，长度 = N（缺省全 0）

**demand_response** 额外可选：
- `demand_limit_kw`：合同需量硬上限（kW），>0 时强制所有时段 `grid <= demand_limit_kw`
- `demand_price_per_kw`：需量电价（元/kW），缺省 10.0
- `dr_signal`：DR 信号数组（0/1），长度 = N；给出时只需量只统计信号=1 的时段

### 3.3 示例

峰谷套利（`sample_request.json`）：

```json
{
  "strategy": "arbitrage",
  "soc": { "initial": 0.5, "min": 0.2, "max": 0.9, "final": 0.5 },
  "battery": { "capacity_kwh": 100.0, "max_charge_kw": 50.0, "max_discharge_kw": 50.0,
               "charge_efficiency": 0.95, "discharge_efficiency": 0.95 },
  "price": [0.30, 0.25, 0.20, 0.22, 0.35, 0.60, 0.80, 0.90, 0.70, 0.55, 0.45, 0.50,
            0.65, 0.75, 0.85, 0.60, 0.40, 0.30, 0.28, 0.32, 0.45, 0.55, 0.50, 0.38],
  "time_step_hours": 1.0
}
```

动态预测优化（`sample_forecast.json`，额外带 `load_kw` / `pv_kw`）和
需求响应（`sample_demand_response.json`，额外带 `dr_signal` / `demand_limit_kw` / `demand_price_per_kw`）见同目录示例文件。

---

## 4. 响应 JSON

```json
{
  "status": "ok",
  "strategy": "arbitrage",
  "solver": "milp_simplex_bb",
  "exact": 1,
  "nodes_used": 3,
  "best_bound": 74.7171,
  "gap_pct": 0,
  "horizon": 24,
  "time_step_hours": 1.0,
  "total_profit_yuan": 74.7171,
  "final_soc": 0.5,
  "plan": [
    { "t": 0, "charge_kw": 0.0, "discharge_kw": 0.0, "soc": 0.5 },
    { "t": 2, "charge_kw": 42.1053, "discharge_kw": 0.0, "soc": 0.9 }
  ]
}
```

字段说明：
- `exact`：1 = 分支定界完成、**已证明最优**；0 = 达到节点上限返回当前最优（未证最优）
- `nodes_used`：分支定界搜索的节点数
- `best_bound`：最优解的上界（= 最优值时说明已证最优；若 `exact=0`，它与 `total_profit_yuan`（或 −`total_cost_yuan`）的差即**积分间隙**）
- `gap_pct`：`(best_bound − 最优目标)/|最优目标| × 100%`，`exact=1` 时为 0
- `plan[]`：与 `price` 一一对应；`t` 时段序号、`charge_kw` 充电功率、`discharge_kw` 放电功率、`soc` 该时段结束 SOC
- **arbitrage**：返回 `total_profit_yuan`
- **forecast / demand_response**：返回 `total_cost_yuan`（= 购电成本 + 需量电费）与 `peak_kw`（最大网购功率）；
  若请求带 `dr_signal` 还返回 `dr_peak_kw`；且 `plan` 每项额外含 `grid_kw`（网购功率，`grid = load - pv + chg - dis`，不允许倒送，grid ≥ 0）

---

## 5. 错误返回

HTTP 400，形如 `{ "status": "error", "message": "..." }`。
常见：请求不是合法 JSON、缺少 `soc`/`battery`/`price`、forecast/DR 缺 `load_kw`、
参数越界、**参数不可行**（如 `demand_limit_kw` 过低导致削峰无解）。

---

## 6. 示例调用

```bash
curl -X POST http://127.0.0.1:8000/api/v1/optimize ^
  -H "Content-Type: application/json" ^
  --data-binary @sample_request.json

python test_client.py sample_forecast.json      # 测试客户端
python verify_plan.py sample_demand_response.json   # 可行性校验
python verify_optimum.py                        # 与 scipy 暴力枚举对照的最优性校验
```

---

## 7. 求解器说明

内部为自研 **MILP 求解器**（`lp.c` 稠密单纯形 + `milp.c` 分支定界）：
每个时段引入 0-1 变量 `u[t]` 禁止同时充放电，SOC 动态、功率上下限、
SOC 上下限、终止 SOC 均为线性约束，构建标准形 MILP 后求解。

