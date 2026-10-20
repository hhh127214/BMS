# -*- coding: utf-8 -*-
"""模拟输入源的「套」—— 一键把两端的**模拟专用字段**填好，并能整体换一档。

=====================================================================
为什么要有这一层
=====================================================================
设备管理页上有四块要填的表（上端 ① 接入参数 / ② IOA 转发表，下端 ① 接入
参数 / ② 点表）。接真机时逐格填是对的；联调时人要做的事其实是「先让整条链
跑起来」，而不是「先想清楚每台设备的模拟器端口」。所以给一个总开关：

    启用模拟源  →  把①的模拟专用字段按「一套」填好，四块整体变模拟色
    换一套      →  端口段整体换一档（1502 → 1602 → 1702 → 1802 → 循环）
    关闭        →  回到真实值

=====================================================================
★ 真实值为什么不会被覆盖（这一整块的前提）
=====================================================================
下端 `device_conn` 里 `host` / `port`（真实）与 `sim_host` / `sim_port`
（模拟）是**两列并存**；上端 `upstream_link` 里 `listen_port` / `common_addr`
是**链路属性**（真实调度与模拟主站连的是同一个监听口），只有 `master_host`
是模拟专用的。

⇒ 启用只写「模拟那一列」，关闭时真实那一列原样还在。
  如果哪天有人图省事让启用去改 `host` / `listen_port`，那就会变成
  「联调完关掉模拟源，现场地址没了」—— 这是本模块唯一要守住的红线。

=====================================================================
★ 为什么「换一套」只换下端端口段
=====================================================================
上端在模拟态下真正变化的只有「谁连进来」（`master_host`）与形态本身；
监听口与 CA 是链路属性，真实调度与模拟主站共用同一个口。本项目里模拟主站
是 `P3/build/iec104_master.exe`，它就跑在本机，所以 `master_host` 恒为
127.0.0.1。

⇒ 换一套时上端的数值确实不变。这不是漏做，是上端没有第二个可换的量；
  面板上要把这句话写出来，否则人会以为主机端没生效。
"""

from typing import Any, Dict, List

import emsdb

# 当前用的第几套。存在 meta 里而不是内存里：进程重启后要知道上次用到哪一套，
# 否则"换一套"每次重启都从第一套开始，等于没换。
M_SIM_PRESET = "sim_preset"

# ---------------------------------------------------------------------
# 「套」的定义
# ---------------------------------------------------------------------
# port_base：下端第 i 台设备的模拟源端口 = port_base + i。
#   为什么是"段"而不是每台一个固定端口：6 台设备 × 4 套 = 24 个端口要记，
#   而"这一套从第几位开始"只有一个数字要记。
UP_MASTER = "127.0.0.1"          # P3/iec104_master.exe 跑在本机

PRESETS: List[Dict[str, Any]] = [
    {"id": 0, "name": "套 1", "port_base": 1502},
    {"id": 1, "name": "套 2", "port_base": 1602},
    {"id": 2, "name": "套 3", "port_base": 1702},
    {"id": 3, "name": "套 4", "port_base": 1802},
]

# 一台设备一个端口，最多给多少台留位置（超出就从段尾继续加，不做截断）
PORT_STEP = 1


def preset_count() -> int:
    return len(PRESETS)


def preset_of(i: int) -> Dict[str, Any]:
    """取第 i 套（越界时按模回绕 —— 换一套是循环的，不该报错）。"""
    if not PRESETS:
        return {"id": 0, "name": "套 1", "port_base": 1502}
    return PRESETS[i % len(PRESETS)]


def index_read(conn) -> int:
    try:
        v = int(emsdb.get_meta(conn, M_SIM_PRESET) or 0)
    except (TypeError, ValueError):
        v = 0
    return v % max(1, len(PRESETS))


def index_write(conn, i: int) -> int:
    n = i % max(1, len(PRESETS))
    emsdb.set_meta(conn, M_SIM_PRESET, str(n))
    return n


def next_index(conn) -> int:
    return (index_read(conn) + 1) % max(1, len(PRESETS))


# ---------------------------------------------------------------------
# 计划：给定套号，算出两端该填什么
# ---------------------------------------------------------------------
def plan_downstream(conn, i: int) -> List[Dict[str, Any]]:
    """下端每一台该填的模拟源地址。

    ★ **按 `device` 台账排，不按 `device_conn`**：
      「自动填好」的意思就是整张表填齐 —— 而 `device_conn` 里没行的设备只是
      「还没登记过接入参数」（现场很常见：先建台账、后接线），它们同样是台账里
      的一台，启用模拟源时一样要给它一个模拟器地址。
      反过来若按 `device_conn` 排，就会出现「6 台设备只有 1 台被填了端口」——
      那不叫自动填好，叫自动填了个寂寞。
    """
    p = preset_of(i)
    base = int(p["port_base"])
    # 台账里的顺序就是端口顺序：device_id 升序，重启后仍然一致。
    rows = emsdb.query(conn, "SELECT device_id FROM device ORDER BY device_id")
    out = []
    for k, r in enumerate(rows):
        out.append({
            "device_id": r["device_id"],
            "sim_host": UP_MASTER,
            "sim_port": base + k * PORT_STEP,
        })
    return out


def plan_upstream(_conn, _i: int) -> Dict[str, Any]:
    """上端该填的模拟主站地址（见文件头的说明：只有一个量）。"""
    return {"master_host": UP_MASTER}


def plan(conn, i: int) -> Dict[str, Any]:
    p = preset_of(i)
    return {
        "preset": p["id"],
        "preset_name": p["name"],
        "port_base": p["port_base"],
        "downstream": plan_downstream(conn, i),
        "upstream": plan_upstream(conn, i),
    }


def note() -> str:
    return ("模拟源只改「模拟那一列」：下端写 sim_host / sim_port，上端写 "
            "模拟主站地址。真实源填的地址、端口、监听口与 CA 原样保留 —— "
            "关掉模拟源就回来了。")
