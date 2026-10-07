@echo off
REM =====================================================================
REM  17\ build_test —— 设备全点表与高保真模拟器：编译 6 个测试并全部运行
REM
REM  对应缺口：模拟器与设备接入梳理.md §10.2 B 类（通路缺失）+ §10.3 C 类（规模） 
REM
REM    B2  设备全点表不存在            → src/device_point_table.h   （107 点）
REM    B3  Python 模拟器不存在          → src/{meter,bms,pcs}_sim.h  （C++ 侧，可注入）
REM    B4  电表模拟器 = 两个 sin()      → src/meter_sim.h           （三相/电能/需量）
REM    B5  BMS 模拟器只有 6 点          → src/bms_sim.h             （簇/单体/温度/限值）
REM    C1  批量写 API 存在但零调用      → src/publish_pipeline.h + rtdb_batch_writer.h
REM    C2  无变化上传（死区）           → DeadbandPublisher
REM    C3  无分级节拍                   → TieredScheduler
REM    C4  两层点表 + 映射表未定义      → device_point_table.h + point_mapping.h
REM    C5  点表是编译期常量             → point_table_loader.h（JSON 往返）
REM
REM  六层测试，职责不重叠： 
REM
REM    test_point_table.exe    两层点表结构（C4） 
REM        · 107 点全点表：点名/单位/量程/默认值/所属设备/点区 逐点自检
REM        · 40 点抽象表 ↔ 全点表映射：DIRECT/COMPOSED/SINK/EXTERNAL 四类来源
REM        · ★ 索引与数组下标一致（D3 纪律：三份点表要对账） 
REM
REM    test_point_config.exe   点表配置化（C5） 
REM        · 内置表 → JSON → 再加载回来，逐点逐字段一致（往返不动点） 
REM        · 改量程/单位必须生效；非法 JSON / 缺字段 / 重名 / 量程矛盾 → 显式报错
REM        · 追加点不破坏既有索引（追加点只能落在末尾，见下方纪律） 
REM
REM    test_sim_meter.exe      电表模拟器（B4） 
REM    test_sim_bms.exe        BMS 模拟器（B5） 
REM    test_sim_pcs.exe        PCS 模拟器（B3） 
REM        · 三个都要求"六类数据源全都在动"：不许有恒为常量的量
REM        · 用断言证明关键量的变化量 > 阈值（静止工况也要有纹波）
REM
REM    test_scale_rtdb.exe     规模化流水线（C1/C2/C3） 
REM        · 分级节拍 → 死区 → 批量写 三者组合，并给出量化对比 
REM        · T10 真的把点写进 RT_DB 段（段建不出来时显式 SKIPPED，不是静默跳过）
REM
REM  ---------------------------------------------------------------------
REM  ★ 三条纪律（每条都有会红的断言守着，见 src/publish_pipeline.h 文件头）： 
REM
REM    1. 死区绝不能吃掉安全位 —— 禁充放/故障/通信位死区必须为 0 = 永远发布。 
REM       被抑制不是"少发一帧"，是**安全链断了**（对应 §10.1 A1）。 
REM    2. 冷启动必须全部先跑一次 —— 慢档点若等第一个周期才发，上电头几秒 
REM       RT_DB 里这些点是"从未收到过"，而"没写过"与"写过 0"不可区分。 
REM    3. 批量写要么整批成功、要么整批拒绝 —— 半批写入会让"这一拍的点集"
REM       处于既不是旧值也不是新值的中间态。 
REM
REM  ---------------------------------------------------------------------
REM  ★ 为什么点表要"两层"： 
REM    算法只认第 2 层（EMS 抽象视图，40 点），名字不许出现在算法里（P0 纪律）；
REM    设备/协议只认第 1 层（107 点，忠于厂家点表）。两层之间必须有**显式映射**， 
REM    否则"某个单体过温"这种信息永远进不到 EMS（§5.3）。 
REM
REM  ★ 为什么新增点只能追加在末尾：
REM    索引是**运行时坐标**，段里的下标由登记顺序决定。中间插入一个点， 
REM    其后所有点的索引整体偏移 1 —— 编译期不报错，运行期按索引取到隔壁点。 
REM    所以 DP_APPEND_BEGIN 之后才是可追加区，既有枚举一个都不能动。 
REM
REM  ---------------------------------------------------------------------
REM  本模块**零第三方依赖**，不需要 Python / pymodbus / Node。 
REM  唯一的"环境相关"是 T10 要建 Windows 页文件映射段；建不出来时它会打印
REM  SKIPPED=1 并以 0 退出 —— **必须每次都打印**，否则"确实跑了"在日志里
REM  没有正面证据，"静默跳过"与"真的跑过"不可区分（§4.5）。 
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0\.."

if not exist build mkdir build

REM ---------- [1/3] 先无条件重编三个 C 源 ----------
REM  ★ 无条件重编，**不要**改成 `if not exist`： 
REM    点表扩容时头文件变了而 .o 没重编，会出现「同名点表两套内容」—— 
REM    编译期不报错，运行期按名字/索引取点错位。这与 13/ 同一个坑。 
gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_point_table.c -o build\ems_point_table.o
if errorlevel 1 ( echo [FAIL] ems_point_table.c compile error & endlocal & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\vendor\rt_db ^
    -c ..\07\vendor\rt_db\rt_db_api.c -o build\rt_db_api.o
if errorlevel 1 ( echo [FAIL] rt_db_api.c compile error & endlocal & exit /b 1 )

gcc -std=c11 -O2 -I ..\07\src\rtdb -I ..\07\vendor\rt_db ^
    -c ..\07\src\rtdb\ems_rt_db_setup.c -o build\ems_rt_db_setup.o
if errorlevel 1 ( echo [FAIL] ems_rt_db_setup.c compile error & endlocal & exit /b 1 )

REM  ---------- [2/3] 编译 + 运行 6 个测试 ----------
set "INC=-I src -I tests -I ..\07\src\rtdb -I ..\07\vendor\rt_db"
set "LNK=build\ems_point_table.o"
set "LNK_RTDB=build\ems_point_table.o build\rt_db_api.o build\ems_rt_db_setup.o"

set NFAIL=0
set TOTAL=0
set TOTFAIL=0
set TOTSKIP=0

call :run test_point_table  tests\test_point_table.cpp
call :run test_point_config tests\test_point_config.cpp
call :run test_sim_meter    tests\test_sim_meter.cpp
call :run test_sim_bms      tests\test_sim_bms.cpp
call :run test_sim_pcs      tests\test_sim_pcs.cpp
call :run test_scale_rtdb   tests\test_scale_rtdb.cpp rtdb

REM  ---------- [3/3] 收尾 ----------
echo.
if not "!NFAIL!"=="0" (
    echo [FAIL] 17\ : !NFAIL! test exe^(s^) did not pass 
    echo        assertions total: PASS=!TOTAL!  FAIL=!TOTFAIL!  SKIPPED=!TOTSKIP!
    endlocal & exit /b 1
)
echo [OK] 17\ device point table ^& hi-fi simulators: 6 test exe^(s^) all passed
echo      assertions total: PASS=!TOTAL!  FAIL=0  SKIPPED=!TOTSKIP!
if not "!TOTSKIP!"=="0" (
    echo      [SKIP] !TOTSKIP! assertion^(s^) explicitly skipped ^(environment limit, not a defect^) 
    echo             printed on purpose: "skipped" must stay distinguishable from "ran". 
)
endlocal & exit /b 0


REM =====================================================================
REM  :run —— 编译一个测试并运行它 
REM    %1 = 测试名（同时是 build\<名>.exe） 
REM    %2 = 源码路径
REM    %3 = 非空且为 rtdb 时，额外链 RT_DB 三个 .o
REM
REM  ★ 只用裸文件名与相对路径做参数（本仓库根路径含括号 D:\wb(cn)\BMS）。 
REM  ★ 块内读取路径语义的变量一律 !VAR! 延迟展开（见 §4.2 与 fix_bat_encoding.py）。 
REM =====================================================================
:run
set "TNAME=%~1"
set "TSRC=%~2"
set "TLNK=!LNK!"
if /i "%~3"=="rtdb" set "TLNK=!LNK_RTDB!"
set "TEXE=build\!TNAME!.exe"
set "TLOG=build\!TNAME!.stdout.log"
set "TERR=build\!TNAME!.stderr.log"

echo.
echo === !TNAME! ===
g++ -std=c++17 -Wall -O2 !INC! "!TSRC!" !TLNK! -o "!TEXE!"
if errorlevel 1 (
    echo [FAIL] !TNAME! compile error
    set /a NFAIL+=1
    exit /b 0
)

REM  标准输出进日志、错误输出单独进另一份日志：
REM  测试的 EXPECT 失败行走 stderr，混在一起会把 FAIL 行插到用例标题中间，
REM  反而看不清是哪条用例红的。分开后先看用例，再看失败清单。 
set "TRC=0"
"!TEXE!" > "!TLOG!" 2> "!TERR!"
if errorlevel 1 set "TRC=1"

type "!TLOG!"
if "!TRC!"=="1" (
    echo ---------- !TNAME! stderr ----------
    type "!TERR!"
    echo [FAIL] !TNAME! has failing assertions
    set /a NFAIL+=1
)

REM  从 "PASS=n FAIL=m SKIPPED=k" 这一行里把三个数累加起来。 
REM  ★ 全靠这一行做全量基线，所以每个测试的 main 末尾都必须印它（§3）。 
for /f "tokens=1,2,3 delims= " %%A in ('findstr /b "PASS=" "!TLOG!" 2^>nul') do (
    for /f "tokens=2 delims==" %%P in ("%%A") do set /a TOTAL+=%%P
    for /f "tokens=2 delims==" %%F in ("%%B") do set /a TOTFAIL+=%%F
    for /f "tokens=2 delims==" %%Q in ("%%C") do set /a TOTSKIP+=%%Q
)
exit /b 0
