@echo off
REM =====================================================================
REM  15/ 平台界面：静态契约自检（Node，零依赖） 
REM
REM  15/ 是纯静态前端，没有编译期检查 —— 它唯一的"编译"是浏览器加载。 
REM  所以这里用 Node 把三类最容易静默出错的东西做成断言： 
REM
REM    A 文件齐备     引用的文件在不在、顺序对不对、能不能离线打开 
REM    B 接口契约     前端调的每个 /api/* —— 后端 14/src/api.py 必须真有 
REM    C 页面与权限   菜单 / 页面函数 / 角色门槛 三者必须一致 
REM
REM  为什么值得单独做：前端最典型的失败是"首页能开、某个子页面接口 404"， 
REM  浏览器只会在 console 里留一行红字，不点开那个页面就永远发现不了。 
REM  静态自检把这个发现时机从"上线后"提前到"构建时"。 
REM
REM  Node 定位顺序：EMS_NODE → PATH 上的 node。 
REM    都没有时打印 SKIP 并以 0 退出 —— 环境缺失不是代码缺陷， 
REM    但必须显式喊出来，否则"静默跳过"与"真的跑过"在输出上不可区分。 
REM
REM  例：
REM    scripts\run_selftest.bat
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\..\.."

REM  ★ 必须用延迟展开 !VAR!，不能用 %VAR% —— 
REM    仓库路径可能含括号（例如 D:\wb(cn)\BMS）。cmd 解析 "if (...)" 块时， 
REM    会把 %VAR% 展开结果里的 ( ) 当成子表达式括号，块结构当场错乱， 
REM    整个脚本在**执行任何一行之前**就报「此时不应有 ...」并以 255 退出。 
REM    延迟展开发生在解析之后，展开结果不再参与语法分析，故安全。 
set NODE=!EMS_NODE!
if "!NODE!"=="" set NODE=node

echo ============================================
echo  15/ 平台界面静态契约自检
echo ============================================

"!NODE!" --version >nul 2>nul
if errorlevel 1 (
    echo [SKIP] 未找到 Node 运行时，EMS_NODE 与 PATH 上都没有 
    echo        这不是代码缺陷，但本次 15\ 的静态断言**未执行** —— 
    echo        全量口径因此少一档，报告里必须按"未执行"口径写。 
    echo        装好 Node 后重跑本脚本即可补齐。 
    endlocal
    exit /b 0
)

"!NODE!" 15\tests\selftest.js %*
if errorlevel 1 (
    echo [FAIL] 15/ 静态契约自检未通过
    endlocal
    exit /b 1
)
echo [OK] 15/ 静态契约自检通过
endlocal
