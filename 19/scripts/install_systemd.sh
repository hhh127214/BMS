#!/usr/bin/env bash
# =====================================================================
# 19/scripts/install_systemd.sh — 在 Linux（XM3568 / aarch64 Debian12）上
#   把 EMS 运行时注册为 systemd 服务，并写入 Linux 版进程清单。
#
# 前提：
#   1) 已跑过 bash scripts/build_linux.sh（产物在 11/build、P3/build）。
#   2) 本机有 systemd（Debian 12 默认有）。写 /etc/systemd 需要 root。
#   3) 如需接真实 RS485 设备，先配 ser2net（见 19/conf/ser2net.conf.example）。
#      ★ 当前 11/src/main_device.cpp 用的是 DeviceSideSim（进程内模拟六路
#        数据源），**并不真实连接 13/ Modbus 主站 / 21/ DL/T645 主站**；
#        所以 ser2net 端口此刻无人消费。真实设备接入是独立工作流
#        （把 13/21 主站接进 device_side 进程），不在本次模型 A 部署范围。
#
# 用法：
#   sudo bash 19/scripts/install_systemd.sh [--root /path/to/ems]
#   --root 默认 = 仓库根（脚本上级两级）。也可用环境变量 EMS_SITE_ROOT。
# =====================================================================
set -euo pipefail

# ---------------------------------------------------------------
# 0. 解析安装根
# ---------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEF_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOT="${EMS_SITE_ROOT:-$DEF_ROOT}"
while [ $# -gt 0 ]; do
  case "$1" in
    --root) ROOT="$2"; shift 2;;
    *) echo "[WARN] 忽略未知参数: $1"; shift;;
  esac
done
ROOT="$(cd "$ROOT" && pwd)"
echo "=== EMS systemd 安装 (root=$ROOT) ==="

# ---------------------------------------------------------------
# 1. 必须 root（写 /etc/systemd）
# ---------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
  echo "[FAIL] 需要 root 权限写 /etc/systemd（请用 sudo 运行）"; exit 1
fi

# ---------------------------------------------------------------
# 2. 构建产物自检
#    ★ 11/ 三进程传 0 会立即退出（循环条件 0<N），所以这里只查存在+可执行，
#      不查参数；参数正确性由 supervisor.linux.json 保证。
# ---------------------------------------------------------------
need=( 11/build/rtdb_initializer 11/build/device_side 11/build/ems_side P3/build/iec104_gateway )
miss=0
for b in "${need[@]}"; do
  if [ ! -x "$ROOT/$b" ]; then
    echo "  [缺失] $ROOT/$b"; miss=1
  fi
done
if [ "$miss" -ne 0 ]; then
  echo "[FAIL] 构建产物不全。请先在目标机跑： bash scripts/build_linux.sh"
  exit 1
fi
echo "  [OK] 4 个运行时产物齐备"

# ---------------------------------------------------------------
# 3. 建必要目录
#    log/            守护器与各子进程日志
#    19/build/       设备/EMS 报告落盘（--report 相对 root）
#    config/point-map/  网关上端链路 + IOA 转发表（15/ 界面导出填充）
# ---------------------------------------------------------------
mkdir -p "$ROOT/log" "$ROOT/19/build" "$ROOT/config/point-map" "$ROOT/conf"
echo "  [OK] 目录就绪：log / 19/build / config/point-map"

# ---------------------------------------------------------------
# 4. 写 Linux 版进程清单（去 .exe，含 iec104_gateway）
# ---------------------------------------------------------------
SVC_JSON_TPL="$ROOT/19/conf/supervisor.linux.json"
if [ ! -f "$SVC_JSON_TPL" ]; then
  echo "[FAIL] 找不到模板 $SVC_JSON_TPL"; exit 1
fi
cp "$SVC_JSON_TPL" "$ROOT/conf/supervisor.json"
echo "  [OK] conf/supervisor.json（Linux 4 进程清单）已写出"

# ---------------------------------------------------------------
# 5. systemd 单元：把 __EMS_ROOT__ 占位符换成真实根
#    （用 | 作 sed 分隔符，避免路径里的 / 转义）
# ---------------------------------------------------------------
SVC_SRC="$ROOT/19/conf/ems_site.service"
if [ ! -f "$SVC_SRC" ]; then
  echo "[FAIL] 找不到单元模板 $SVC_SRC"; exit 1
fi
sed "s|__EMS_ROOT__|$ROOT|g" "$SVC_SRC" > /etc/systemd/system/ems_site.service
echo "  [OK] /etc/systemd/system/ems_site.service 已生成（EMS_ROOT=$ROOT）"

# ---------------------------------------------------------------
# 6. 注册并启动
# ---------------------------------------------------------------
systemctl daemon-reload
systemctl enable --now ems_site
echo "  [OK] ems_site 已 enable --now"

# ---------------------------------------------------------------
# 7. 后续核对步骤
# ---------------------------------------------------------------
echo
echo "=== 后续核对 ==="
echo "  systemctl status ems_site              # 守护器是否起来"
echo "  journalctl -u ems_site -f             # 守护器日志"
echo "  ls $ROOT/log                          # 各子进程 .out.log + supervisor.log"
echo "  python3 $ROOT/19/src/ems_supervisor.py --root $ROOT check   # 进程清单 preflight"
echo
echo "  ★ RS485 真实设备接入：先配 ser2net（19/conf/ser2net.conf.example）。"
echo "    但当前 device_side 是 DeviceSideSim（模拟六路），真实 13/21 主站"
echo "    接入是独立工作流，不在本次模型 A 部署范围内。"
echo "  ★ 上端链路 / IOA 转发表：把 15/ 界面导出的文件放到"
echo "    $ROOT/config/point-map/（active.upstream / active.pointmap.csv），"
echo "    否则网关用内置默认表 + 默认监听口 2404。"
