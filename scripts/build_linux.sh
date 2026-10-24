#!/usr/bin/env bash
# =====================================================================
#  scripts/build_linux.sh — EMS 运行时在 Linux（XM3568 / aarch64 或 x86_64）的构建
#
#  对应 Windows 侧：scripts/build_all.bat 里 11/ + P3(+vendor/lib60870) 两段。
#  本脚本只编**现场运行时要跑的运行时**产物（与 19/ 守护器拉起的进程对应）：
#    11/build/rtdb_initializer      共享内存段创建+保活
#    11/build/device_side           设备侧进程（拉动 13/ Modbus · 21/ 645 · rt_db）
#    11/build/ems_side              EMS 侧进程（全栈调度闭环）
#    P3/build/iec104_gateway        IEC104 从站网关（上行 SCADA）
#    P3/build/iec104_master        主站模拟器（联调自测用）
#    vendor/lib60870/build/lib60870.a  第三方 IEC60870 静态库
#
#  ★ 平移纪律（与 Windows build.bat 一一对应，不发明编译参数）：
#    · 去掉 -lws2_32 / -liphlpapi / -lbcrypt（POSIX socket 属 libc），
#      改加 -pthread（std::thread 与 lib60870 线程都需要）。
#    · 去掉 MinGW 专属的 -D__USE_MINGW_ANSI_STDIO=1（glibc 不需要）。
#    · vendor/lib60870：Windows 下手选 23 文件是因为"本机没有 make"；
#      Linux 上直接用上游自带 Makefile（明文 104，不含 TLS），更 faithful。
#
#  ⚠ 验证状态：本脚本为**忠实平移**，尚未在目标 Linux 上实跑 build-verify。
#    需在有 gcc/g++/make 的 Linux（XM3568 本机或 x86_64 开发机）上跑一次
#    `bash scripts/build_linux.sh` 确认产物齐全。这与 13/modbus_tcp_client.h、
#    16/net_base.h、21/dlt645_tcp_link.h 的 POSIX 分支同等待遇——它们本就
#    只在 Linux 上才被编译。
# =====================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== EMS Linux build (root=$ROOT) ==="

# ---------------------------------------------------------------
# 0. 工具链探测
# ---------------------------------------------------------------
GCC=${CC:-gcc}
GPP=${CXX:-g++}
echo "gcc=$GCC  g++=$GPP"
"$GCC" --version | head -1
"$GPP" --version | head -1

# ---------------------------------------------------------------
# 1. RT_DB C 实现（三个 .c，无条件重编——点表扩容后头文件变了必须跟着重编）
# ---------------------------------------------------------------
echo "=== [1/5] RT_DB C library ==="
RTDBOBJS="11/build/rt_db_api.o 11/build/ems_point_table.o 11/build/ems_rt_db_setup.o"
mkdir -p 11/build
$GCC -std=c11 -O2 -Wall -Werror=implicit-function-declaration -D_DEFAULT_SOURCE -I 07/vendor/rt_db \
    -c 07/vendor/rt_db/rt_db_api.c -o 11/build/rt_db_api.o
$GCC -std=c11 -O2 -Wall -Werror=implicit-function-declaration -D_DEFAULT_SOURCE -I 07/src/rtdb -I 07/vendor/rt_db \
    -c 07/src/rtdb/ems_point_table.c -o 11/build/ems_point_table.o
$GCC -std=c11 -O2 -Wall -Werror=implicit-function-declaration -D_DEFAULT_SOURCE -I 07/src/rtdb -I 07/vendor/rt_db \
    -c 07/src/rtdb/ems_rt_db_setup.c -o 11/build/ems_rt_db_setup.o

# ---------------------------------------------------------------
# 2. vendor/lib60870 静态库（Linux：用上游自带 Makefile）
# ---------------------------------------------------------------
echo "=== [2/5] lib60870 static library ==="
LIB60870_A=""
if [ -f vendor/lib60870/lib60870.a ]; then
    LIB60870_A=vendor/lib60870/lib60870.a
elif [ -f vendor/lib60870/build/lib60870.a ]; then
    LIB60870_A=vendor/lib60870/build/lib60870.a
elif [ -f vendor/lib60870/build/liblib60870.a ]; then
    # 上游 target_system.mk 的 LIB_NAME = build/liblib60870.a（双 lib 是上游命名，非笔误）
    LIB60870_A=vendor/lib60870/build/liblib60870.a
fi
if [ -z "$LIB60870_A" ]; then
    echo "  building via make (plaintext, no TLS/examples)..."
    ( cd vendor/lib60870 && make ) || {
        echo "[FAIL] lib60870 make failed"; exit 1; }
    if [ -f vendor/lib60870/lib60870.a ]; then
        LIB60870_A=vendor/lib60870/lib60870.a
    elif [ -f vendor/lib60870/build/lib60870.a ]; then
        LIB60870_A=vendor/lib60870/build/lib60870.a
    elif [ -f vendor/lib60870/build/liblib60870.a ]; then
        LIB60870_A=vendor/lib60870/build/liblib60870.a
    else
        echo "[FAIL] lib60870.a not found after make"; exit 1
    fi
fi
echo "  lib60870 -> $LIB60870_A"

# ---------------------------------------------------------------
# 3. 11/ 三进程（initializer / device_side / ems_side）
#     INC 与 Windows 11/scripts/build.bat 完全一致（扁平 -I 解析跨模块头文件）
# ---------------------------------------------------------------
echo "=== [3/5] 11/ integration processes ==="
INC11="-I 11/src -I 04/src -I 05/src -I 06/src -I 07/src -I 08/src -I P2/src -I 07/src/rtdb -I 07/vendor/rt_db"
$GPP -std=c++17 -Wall -O2 -I 11/src -I 07/src/rtdb -I 07/vendor/rt_db \
    11/src/main_initializer.cpp $RTDBOBJS -pthread -o 11/build/rtdb_initializer
$GPP -std=c++17 -Wall -O2 $INC11 \
    11/src/main_device.cpp $RTDBOBJS -pthread -o 11/build/device_side
$GPP -std=c++17 -Wall -O2 $INC11 \
    11/src/main_ems.cpp $RTDBOBJS -pthread -o 11/build/ems_side

# ---------------------------------------------------------------
# 4. P3/ IEC104 网关（链接 lib60870.a + rt_db 目标文件）
#    INC 与 Windows P3/scripts/build.bat 一致；-DNOMINMAX 保留
#    （lib60870 的 win32 HAL 头会带 <windows.h>，但 Linux 走 linux HAL，
#      NOMINMAX 对 POSIX 无害，保留以与 Windows 行为一致）。
# ---------------------------------------------------------------
echo "=== [4/5] P3/ IEC104 gateway ==="
mkdir -p P3/build
INCP3="-I P3/src \
 -I vendor/lib60870/config \
 -I vendor/lib60870/src/inc/api \
 -I vendor/lib60870/src/inc/internal \
 -I vendor/lib60870/src/common/inc \
 -I vendor/lib60870/src/hal/inc \
 -I 07/src/rtdb \
 -I 07/vendor/rt_db"
LNKP3="$LIB60870_A $RTDBOBJS -pthread"
$GPP -std=c++17 -Wall -O2 -DNOMINMAX $INCP3 \
    P3/src/main_gateway.cpp $LNKP3 -o P3/build/iec104_gateway
$GPP -std=c++17 -Wall -O2 -DNOMINMAX $INCP3 \
    P3/src/main_master.cpp $LNKP3 -o P3/build/iec104_master

# ---------------------------------------------------------------
# 5. 现场点表核对工具（可选；与 13/21/07 的 TCP 链路配套）
#    默认不编；设 BUILD_TOOLS=1 开启（平移各自 build.bat，去 -lws2_32）
# ---------------------------------------------------------------
if [ "${BUILD_TOOLS:-0}" = "1" ]; then
    echo "=== [5/5] field tools (BUILD_TOOLS=1) ==="
    mkdir -p 13/build 21/build 07/build
    $GCC -std=c11 -O2 -Wall -Werror=implicit-function-declaration -D_DEFAULT_SOURCE -I 07/src/rtdb -I 07/vendor/rt_db \
        -c 07/src/rtdb/ems_point_table.c -o 13/build/ems_point_table.o
    $GPP -std=c++17 -Wall -O2 -I 13/src -I 07/src/rtdb -I 04/src \
        13/src/main_probe.cpp 13/build/ems_point_table.o -pthread -o 13/build/modbus_probe
    $GPP -std=c++17 -Wall -Wextra -O2 -I 21/src \
        21/src/main_meter645.cpp -pthread -o 21/build/meter645
    $GPP -std=c++17 -Wall -O2 -I 07/src -I 07/src/rtdb -I 04/src -I 05/src \
        -I 06/src -I 08/src -I 13/src -I 21/src -I 07/vendor/rt_db \
        07/src/main_field.cpp $RTDBOBJS -pthread -o 07/build/main_field
fi

echo
echo "=== BUILD OK ==="
ls -l 11/build/rtdb_initializer 11/build/device_side 11/build/ems_side \
      P3/build/iec104_gateway P3/build/iec104_master "$LIB60870_A"
echo "next: 19/scripts/install_systemd.sh   (注册 systemd + 写 Linux supervisor 清单)"
