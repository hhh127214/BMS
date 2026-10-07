@echo off
REM =====================================================================
REM  15/ 平台界面：起后端服务并托管界面 
REM
REM  为什么需要这一步：15/ 是纯静态前端，但**不能双击 index.html 打开** —— 
REM    界面要调 24 个 /api/* 接口，file:// 协议下浏览器按跨源直接拦掉， 
REM    页面会停在"加载失败"。必须由 14/ 的后端服务托管。 
REM
REM  依赖数据库有数据。若 14\data\ems.db 不存在，先跑： 
REM    scripts\import_sim.bat        （把 10\ 的 24h 仿真产物导进库） 
REM
REM  例：
REM    scripts\run_ui.bat                  默认 http://127.0.0.1:8765/
REM    scripts\run_ui.bat --port 9000      换端口（少用；本项目固定 8765）
REM    scripts\run_ui.bat --host 0.0.0.0   仅限内网演示，注意先改默认口令 
REM
REM  界面默认账号：admin/admin123 · operator/operator123 · viewer/viewer123
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."

REM  ★ 必须用延迟展开 !VAR!：仓库路径可能含括号（D:\wb(cn)\...），
REM    而 %VAR% 在 "if (...)" 块内展开时，其中的 ( ) 会被 cmd 当成
REM    子表达式括号，块结构错乱 → 脚本一行都没跑就报「此时不应有 ...」。 
set PY=!EMS_PYTHON!
if "!PY!"=="" set PY=python

if not exist 14\data\ems.db (
    echo [warn] 14\data\ems.db 不存在 —— 界面能开，但所有页面都会是空的。 
    echo        先跑：scripts\import_sim.bat
    echo.
)

echo ============================================
echo  15/ 平台界面
echo ============================================
echo  后端     : 14\src\server.py   静态托管 15\
echo  浏览器   : http://127.0.0.1:8765/
echo  接口自检 : http://127.0.0.1:8765/api/health
echo  默认账号 : admin / admin123
echo.
echo  停止服务 : Ctrl+C
echo ============================================
echo.

"!PY!" 14\src\server.py --static 15 %*

endlocal & exit /b %errorlevel%
