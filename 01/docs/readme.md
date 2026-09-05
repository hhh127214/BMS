# battery_optimizer_c

纯 C 实现的电池分时充放电优化 Web 服务（**C 策略：峰谷套利 / 动态预测优化 / 需求响应**）。
文件名仅含 a-z / 0-9 / 下划线。

## 功能
- `POST /api/v1/optimize`：接收 A/B 组的 SOC、约束、电价/负荷/光伏预测 JSON，
  返回每个时段的充放电功率计划。
- 三种策略：
  - `arbitrage`          峰谷套利（最大化套利收益）
  - `forecast`           动态预测优化（最小化购电成本，滚动优化）
  - `demand_response`    需求响应（削峰，最小化需量电费 + 购电成本）
- `GET /health`：健康检查。
- 内部为自研 MILP 求解器（稠密单纯形 + 分支定界），0-1 变量禁止同时充放电。

## 快速开始
1. 编译：`build.bat`
2. 启动：`run.bat`（默认 0.0.0.0:8000，可带端口参数）
3. 测试：
   - `curl -X POST http://127.0.0.1:8000/api/v1/optimize -H "Content-Type: application/json" --data-binary @sample_request.json`
   - 或 `python test_client.py sample_forecast.json`

## 文件
- `main.c`               HTTP 服务器（Winsock2），路由 POST /api/v1/optimize
- `json_util.c/h`        极简 JSON 解析器
- `solver.c/h`           MILP 模型构建（三种策略）+ 响应生成
- `lp.c/h`               LP 单纯形求解器（big-M 人工变量）
- `milp.c/h`             MILP 分支定界求解器
- `build.bat` / `run.bat` 编译与启动脚本
- `api.md`               接口文档（请求/响应格式、字段表、示例）
- `sample_request.json` / `sample_forecast.json` / `sample_demand_response.json` 示例请求
- `test_client.py`       测试客户端
- `verify_plan.py`       可行性校验
- `verify_optimum.py`    与 scipy 暴力枚举对照的最优性校验
- `bench_96.py`          96 时段（24h×15min）大规模性能基准
- `test_solver.c`        LP/MILP 引擎单元测试
- `requirements.txt`     依赖说明（纯 C，无 pip 依赖）
- `report.md`            项目开发报告
- `report.docx`         报告 Word 版（用 `python make_docx.py` 重新生成）
- `report.pptx`         项目 PPT（25 页，风格同作业汇报模板；含测试图表，用 `python make_ppt.py` 生成）
- `strategy_present.pptx`  三策略专题 PPT（15 页：三个策略是什么、怎么实现；用 `python make_strategy_ppt.py` 生成）
- `tech_present.pptx`     技术深挖 PPT（16 页：约束构建与求解器详解，附真实 C 代码片段；用 `python make_tech_ppt.py` 生成）
- `make_strategy_ppt.py`  三策略专题 PPT 生成脚本
- `make_tech_ppt.py`      技术深挖 PPT 生成脚本
- `make_docx.py`        report.md → report.docx 转换脚本
- `make_charts.py`      生成测试图表 PNG（需服务运行，输出到 `charts/`）
- `make_ppt.py`         生成 report.pptx（嵌入 charts/*.png）

## 验证结果
- 三个示例请求均通过 `verify_plan.py` 可行性校验
- 12 个随机小算例（n=4，三种策略）均与 scipy 暴力枚举最优值一致
- 96 时段（24h×15min）性能：套利 23 ms、预测 0.8 s、需求响应约 15 s，均证明最优（exact=1）

