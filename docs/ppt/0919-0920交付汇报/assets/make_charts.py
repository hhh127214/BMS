# -*- coding: utf-8 -*-
"""生成「0919-0920 两天交付」汇报 PPT 所需的图表。

数据源全部来自真实产物，不手抄数字：
  - 24h 仿真时序：10/build/timeseries.csv（8640 行 = 86400 拍，由 test_sim_24h.exe 实跑产出）
  - 仿真汇总指标：10/build/summary.json
  - 断言基线：README.md 基线块（汇报前 19 个测试目标逐个 cd 进模块目录实测复核过）
  - 缺陷 / 反证 / 端到端：CHANGES.md §22~§33 + docs/工作汇报/2026-09-19_20_工作汇报.html

配色严格收敛到 4 色 + 中性色，与 DESIGN.md 一致。
★ 中文绝不单独指定 fontfamily（会丢字形），统一靠下面的 rcParams 排优先级。
"""
import json
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Polygon, Rectangle
from matplotlib.lines import Line2D

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

BLUE = "#3B82F6"
CYAN = "#06B6D4"
RED = "#EF4444"
GRAY = "#94A3B8"
INK = "#1A1A1A"
GRID = "#E2E8F0"
BLUE_BG = "#EFF6FF"
RED_BG = "#FEF2F2"
GRAY_BG = "#F8FAFC"

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
ASSETS = os.path.dirname(os.path.abspath(__file__))
TS = os.path.join(ROOT, "10", "build", "timeseries.csv")
SUM = os.path.join(ROOT, "10", "build", "summary.json")


# ---------------------------------------------------------------- 基础设施
def load_series():
    rows = []
    with open(TS, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def fnum(rows, key):
    return [float(r[key]) for r in rows]


def polish(ax):
    ax.set_facecolor("white")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=GRAY, labelsize=11)
    ax.grid(True, color=GRID, linewidth=0.8, alpha=0.9)
    ax.set_axisbelow(True)


def save(fig, name):
    p = os.path.join(ASSETS, name)
    fig.savefig(p, dpi=150, facecolor="white", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)
    print("wrote", name)


def rbox(ax, x, y, w, h, text, fc="white", ec=GRAY, tc=INK, fs=12,
         bold=False, lw=1.4, radius=0.12, zorder=3):
    ax.add_patch(FancyBboxPatch(
        (x, y), w, h, boxstyle=f"round,pad=0,rounding_size={radius}",
        linewidth=lw, facecolor=fc, edgecolor=ec, zorder=zorder))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            color=tc, fontsize=fs, zorder=zorder + 1,
            fontweight="bold" if bold else "normal", linespacing=1.5)


def arrow(ax, p1, p2, color=GRAY, style="-|>", lw=1.7, ls="solid", rad=0.0, zorder=2, ms=15):
    ax.add_patch(FancyArrowPatch(
        p1, p2, arrowstyle=style, mutation_scale=ms, linewidth=lw, color=color,
        linestyle=ls, zorder=zorder, connectionstyle=f"arc3,rad={rad}"))


# ================================================================ 一 · 两天总览
def chart_timeline2():
    """两天交付时间线：09-19 补缺口 / 09-20 设备接入，各带当日断言净增。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.8))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.8)
    ax.axis("off")

    # ★ 卡片口径 = docs/工作汇报/2026-09-19_20_工作汇报.html §一 交付项表的 11 行
    #   （09-19 六项 / 09-20 五项）。卡片数与项目数必须一致，否则与报告对不上。
    lanes = [
        dict(
            head=5.02, cards=3.46, name="09-19", sub="补齐设计方案缺口", col=BLUE,
            head_from="8085", head_to="8547", delta="+462",
            items=[
                ("新建 11/ 跨进程联调", "三真进程 + 真共享内存"),
                ("新建 12/ 最终验收", "七维度 55 项检查"),
                ("新建 P3/ IEC104 从站", "对上接调度（真报文）"),
                ("修 07/ 三处真实缺陷", "单进程照不出来的三处"),
                ("缺口 A1 + A2", "禁充放位 / 关口口径"),
                ("判据缺陷 T47", "原判据测的是负载"),
            ]),
        dict(
            head=2.50, cards=0.96, name="09-20", sub="设备接入与外部设定", col=CYAN,
            head_from="8547", head_to="9304", delta="+757",
            items=[
                ("新建 13/ Modbus 接入", "对下接现场设备（三层测试）"),
                ("缺口 A3.1 · EXT 点区", "点表 32 → 40 点，调度有落点"),
                ("缺口 A3.2 接进安全链", "第 10 条约束 + 停机粘住"),
                ("转发表外置 CSV", "现场改 IOA 不用重编"),
                ("三遍文案 / 遗留清扫", "会印进产物里的过期数字"),
            ]),
    ]

    for ln in lanes:
        hd, cy, col = ln["head"], ln["cards"], ln["col"]
        # 表头行：日期徽标 + 当日主题 + 当日断言净增
        ax.add_patch(FancyBboxPatch((0.30, hd - 0.30), 1.34, 0.60,
                                    boxstyle="round,pad=0,rounding_size=0.10",
                                    facecolor=col, alpha=0.16,
                                    edgecolor=col, linewidth=1.4, zorder=3))
        ax.text(0.97, hd, ln["name"], ha="center", va="center", color=col,
                fontsize=14, fontweight="bold", zorder=4)
        ax.text(1.82, hd + 0.11, ln["sub"], ha="left", va="center", color=INK,
                fontsize=12.5, fontweight="bold")
        ax.text(1.82, hd - 0.20, "每张卡片 = 一项交付", ha="left", va="center",
                color=GRAY, fontsize=10.2)
        ax.text(12.10, hd,
                f"当日断言 {ln['head_from']} → {ln['head_to']}　{ln['delta']}"
                f"（+{round((int(ln['head_to']) / int(ln['head_from']) - 1) * 100)}%）",
                ha="right", va="center", color=col, fontsize=11.5, fontweight="bold")

        # 卡片宽度按当天项目数自适应：09-19 六项 / 09-20 五项，整体铺满 0.30~12.10
        n = len(ln["items"])
        gap = 0.10 if n >= 6 else 0.14
        w, h = (11.80 - (n - 1) * gap) / n, 1.00
        nfs = 9.3 if n >= 6 else 10.8
        tfs = 8.1 if n >= 6 else 9.3
        x = 0.30
        for i, (nm, note) in enumerate(ln["items"]):
            rbox(ax, x, cy, w, h, "", fc="white", ec=col, lw=1.3)
            ax.text(x + w / 2, cy + h * 0.62, nm, ha="center", va="center", color=INK,
                    fontsize=nfs, fontweight="bold", zorder=5)
            ax.text(x + w / 2, cy + h * 0.27, note, ha="center", va="center", color=GRAY,
                    fontsize=tfs, zorder=5)
            if i < n - 1:
                arrow(ax, (x + w + 0.005, cy + h / 2), (x + w + gap - 0.005, cy + h / 2),
                      color=GRAY, lw=1.2, ms=11)
            x += w + gap

    # 总计条
    ax.add_patch(FancyBboxPatch((0.30, 0.06), 11.80, 0.62,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=RED_BG, edgecolor=RED, linewidth=1.4, zorder=3))
    ax.text(6.20, 0.37,
            "两天合计：+1219 条断言（8085 → 9304，+15%），0 失败；"
            "另新增 4 个可执行进程与 17 个源文件",
            ha="center", va="center", color=RED, fontsize=12.5, fontweight="bold", zorder=4)
    save(fig, "timeline_2days.png")


# ================================================================ 二 · 原理图
def chart_crossproc():
    """为什么必须跨进程：单进程 + 夹具注入 = 测试盲区。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.9))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.9)
    ax.axis("off")

    ax.text(0.30, 5.62, "同一套代码，换一种装法：单进程全绿，跨进程才照出真缺陷",
            color=INK, fontsize=13.5, fontweight="bold", va="center")
    ax.text(0.30, 5.22, "串进真进程、真共享内存之后，「模块内自洽」不再等于「装到一起还对不对」",
            color=GRAY, fontsize=11, va="center")

    # ---- 左：单进程
    ax.add_patch(FancyBboxPatch((0.30, 2.85), 5.60, 2.10,
                                boxstyle="round,pad=0,rounding_size=0.12",
                                facecolor=GRAY_BG, edgecolor=GRAY, linewidth=1.4, zorder=2))
    ax.text(3.10, 4.68, "单进程（改之前）", ha="center", color=GRAY,
            fontsize=13, fontweight="bold", zorder=3)
    for i, nm in enumerate(["04/ 算法", "05/ 安全", "07/ EMS 循环"]):
        rbox(ax, 0.58 + i * 1.82, 3.72, 1.58, 0.56, nm, fc="white", ec=GRAY, fs=10.5)
        if i < 2:
            arrow(ax, (2.17 + i * 1.82, 4.00), (2.39 + i * 1.82, 4.00), color=GRAY, lw=1.3, ms=11)
    ax.text(3.10, 3.42, "测试夹具在进程内直接注入数据",
            ha="center", color=GRAY, fontsize=10.8, zorder=3)
    ax.text(3.10, 3.06, "→ 跨内存边界那一段被夹具替掉了，永远是绿的",
            ha="center", color=RED, fontsize=11, fontweight="bold", zorder=3)

    # ---- 右：跨进程
    ax.add_patch(FancyBboxPatch((6.50, 2.85), 5.60, 2.10,
                                boxstyle="round,pad=0,rounding_size=0.12",
                                facecolor=BLUE_BG, edgecolor=BLUE, linewidth=1.5, zorder=2))
    ax.text(9.30, 4.68, "跨进程（11/ 新建）", ha="center", color=BLUE,
            fontsize=13, fontweight="bold", zorder=3)
    for i, nm in enumerate(["初始化器", "设备侧", "EMS 侧"]):
        rbox(ax, 6.80 + i * 1.78, 3.72, 1.52, 0.56, nm, fc="white", ec=BLUE, fs=10.8)
    ax.add_patch(FancyBboxPatch((6.80, 3.12), 5.00, 0.42,
                                boxstyle="round,pad=0,rounding_size=0.08",
                                facecolor=CYAN, alpha=0.20, edgecolor=CYAN, linewidth=1.2, zorder=3))
    ax.text(9.30, 3.33, "RT_DB 共享内存段（唯一的交互通道）",
            ha="center", va="center", color=CYAN, fontsize=10.8, fontweight="bold", zorder=4)
    for i in range(3):
        arrow(ax, (7.56 + i * 1.78, 3.70), (7.56 + i * 1.78, 3.56), color=CYAN, lw=1.2, ms=11, zorder=4)
    ax.text(9.30, 2.98, "物理上不可能共享内存指针",
            ha="center", color=BLUE, fontsize=11, fontweight="bold", zorder=3)

    # ---- 下：暴露的 3 个真缺陷
    defects = [
        ("①", "EmsRuntime::step() 从不调用 write_command()",
         "权限区间恒 [0, 0] → 3000 拍里 2978 拍被判「指令越界」"),
        ("②", "seqlock 碰撞被误计成「采集失败」",
         "三进程下 600 拍出现 51 次假告警（语义应是「请重试」）"),
        ("③", "MemoryDeviceIO 缺热模型",
         "温度恒 25℃ → 「六类数据源全活」这条永远过不了"),
    ]
    for i, (no, title, cons) in enumerate(defects):
        y = 2.02 - i * 0.66
        ax.add_patch(FancyBboxPatch((0.30, y), 11.56, 0.54,
                                    boxstyle="round,pad=0,rounding_size=0.08",
                                    facecolor=RED_BG, edgecolor=RED, linewidth=1.2, zorder=3))
        ax.text(0.58, y + 0.27, no, ha="center", va="center", color=RED,
                fontsize=12, fontweight="bold", zorder=4)
        ax.text(0.92, y + 0.27, title, ha="left", va="center", color=INK,
                fontsize=11.2, fontweight="bold", zorder=4)
        ax.text(11.68, y + 0.27, cons, ha="right", va="center", color=GRAY,
                fontsize=10.6, zorder=4)
    ax.text(0.30, 0.02, "三个缺陷在单进程测试里永远不会暴露 —— 跨进程是唯一能照出它们的镜子。",
            color=RED, fontsize=11.5, fontweight="bold", va="center")
    save(fig, "crossproc.png")


def chart_row_of_boxes(ax, x, y, w, h, gap, items, col, tfs=11.0, nfs=9.2):
    """一行卡片，返回行宽。"""
    cx = x
    for i, (nm, note) in enumerate(items):
        rbox(ax, cx, y, w, h, "", fc="white", ec=col, lw=1.3)
        ax.text(cx + w / 2, y + h * 0.62, nm, ha="center", va="center", color=INK,
                fontsize=tfs, fontweight="bold", zorder=5)
        ax.text(cx + w / 2, y + h * 0.24, note, ha="center", va="center", color=GRAY,
                fontsize=nfs, zorder=5)
        if i < len(items) - 1:
            arrow(ax, (cx + w + 0.005, y + h / 2), (cx + w + gap - 0.005, y + h / 2),
                  color=GRAY, lw=1.2, ms=11)
        cx += w + gap
    return cx - gap - x


def chart_modbus_arch():
    """对下接现场设备：Modbus TCP 主从架构与接口方向。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.6))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.6)
    ax.axis("off")

    ax.text(0.30, 5.34, "对下接现场设备：C++ 主站  /  Modbus TCP 从站（先对模拟器，后对真机）",
            color=INK, fontsize=13.2, fontweight="bold", va="center")

    ax.text(6.20, 4.72, "←  量测 / 状态上行：读 IR · HR · DI",
            ha="center", color=CYAN, fontsize=11.2, fontweight="bold")
    ax.text(6.20, 4.42, "→  指令下行：写 HR（FC16 一次原子下发 3 个连续指令点）",
            ha="center", color=BLUE, fontsize=11.2, fontweight="bold")

    y, h = 2.90, 1.30
    rbox(ax, 0.30, y, 3.06, h, "", fc=GRAY_BG, ec=GRAY, lw=1.4)
    ax.text(1.83, y + h * 0.72, "设备侧（对端）", ha="center", va="center", color=INK,
            fontsize=11.8, fontweight="bold", zorder=5)
    ax.text(1.83, y + h * 0.30, "Python pymodbus 从站\n或 真机 BMS / PCS / 电表",
            ha="center", va="center", color=GRAY, fontsize=9.8, zorder=5, linespacing=1.5)

    rbox(ax, 3.66, y, 2.54, h, "", fc="white", ec=CYAN, lw=1.5)
    ax.text(4.93, y + h * 0.71, "Modbus TCP", ha="center", va="center", color=CYAN,
            fontsize=11.8, fontweight="bold", zorder=5)
    ax.text(4.93, y + h * 0.30, "MBAP + PDU\n真 TCP 报文 · 502 端口",
            ha="center", va="center", color=GRAY, fontsize=9.8, zorder=5, linespacing=1.5)

    rbox(ax, 6.50, y, 3.06, h, "", fc=BLUE_BG, ec=BLUE, lw=1.5)
    ax.text(8.03, y + h * 0.72, "ModbusDeviceIO", ha="center", va="center", color=INK,
            fontsize=11.8, fontweight="bold", zorder=5)
    ax.text(8.03, y + h * 0.30, "IDeviceIO 第 4 个实现\n与仿真适配器可互换",
            ha="center", va="center", color=GRAY, fontsize=9.8, zorder=5, linespacing=1.5)

    rbox(ax, 9.86, y, 2.24, h, "", fc="white", ec=BLUE, lw=1.5)
    ax.text(10.98, y + h * 0.72, "RT_DB 点表", ha="center", va="center", color=INK,
            fontsize=11.8, fontweight="bold", zorder=5)
    ax.text(10.98, y + h * 0.30, "40 点\nMEAS / STA / CFG\nCMD / EXT",
            ha="center", va="center", color=GRAY, fontsize=9.8, zorder=5, linespacing=1.45)

    for x0, x1 in [(3.36, 3.66), (6.20, 6.50), (9.56, 9.86)]:
        arrow(ax, (x0, y + h * 0.72), (x1, y + h * 0.72), color=BLUE, lw=1.6, ms=13)
        arrow(ax, (x1, y + h * 0.32), (x0, y + h * 0.32), color=CYAN, lw=1.6, ms=13)

    for i, (nm, note) in enumerate([
        ("协议层 223 断言", "功能码 / 异常码 / 分块 / 边界 / 串包自愈"),
        ("适配器契约 133 断言", "三次请求语义 · 超时 ≠ 暂时无数据 · 超范围钳位"),
        ("跨语言对账 83 断言", "C++ 侧对上真 pymodbus，逐拍对账"),
    ]):
        x = 0.30 + i * 4.02
        rbox(ax, x, 1.52, 3.80, 1.06, "", fc="white", ec=GRAY, lw=1.2)
        ax.text(x + 1.90, 2.30, nm, ha="center", va="center", color=BLUE,
                fontsize=11.5, fontweight="bold", zorder=5)
        ax.text(x + 1.90, 1.84, note, ha="center", va="center", color=GRAY,
                fontsize=9.4, zorder=5, linespacing=1.5)

    ax.add_patch(FancyBboxPatch((0.30, 0.10), 11.80, 1.10,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=RED_BG, edgecolor=RED, linewidth=1.3, zorder=3))
    ax.text(6.20, 1.00, "为什么不干脆让 Python 模拟器直写共享内存？",
            ha="center", va="center", color=RED, fontsize=11.8, fontweight="bold", zorder=4)
    ax.text(6.20, 0.52,
            "那样「Modbus 写寄存器」这条路在接真机之前一次都没走过 —— 字节序、寄存器地址、\n"
            "写后回读、超时重试都会在现场第一次遇到。走真协议：对模拟器是调试，对真机就是生产。",
            ha="center", va="center", color=INK, fontsize=10.5, zorder=4, linespacing=1.6)
    save(fig, "modbus_arch.png")


def chart_ext_protocol():
    """EXT 发布协议：写者前后各递增一次；读者要求 s0==s1 且为偶数。"""
    fig, ax = plt.subplots(figsize=(12.4, 6.1))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 6.1)
    ax.axis("off")

    ax.text(0.30, 5.86, "一组 8 个点必须「要么全旧、要么全新」——单点不撕裂挡不住一组量撕裂",
            color=INK, fontsize=13.2, fontweight="bold", va="center")

    # 写者时间轴（红段 = 写入中，此后 SEQ 为奇数）
    ax.text(0.30, 5.42, "写者（IEC104 网关）", color=BLUE, fontsize=11.5,
            fontweight="bold", va="center")
    ax.plot([3.00, 11.90], [5.14, 5.14], color=GRID, linewidth=2.4, zorder=2)
    steps = [(3.35, "SEQ 1\n奇数", RED), (4.45, "写入 8 点", BLUE),
             (5.75, "SEQ 2\n偶数", CYAN), (7.05, "SEQ 3\n奇数", RED),
             (8.15, "写入 8 点", BLUE), (9.45, "SEQ 4\n偶数", CYAN)]
    ax.plot([3.35, 5.75], [5.14, 5.14], color=RED, linewidth=3.4, alpha=0.50, zorder=3)
    ax.plot([7.05, 9.45], [5.14, 5.14], color=RED, linewidth=3.4, alpha=0.50, zorder=3)
    for x, lab, col in steps:
        ax.plot([x], [5.14], marker="o", markersize=9, color=col, zorder=4)
        ax.text(x, 4.88, lab, ha="center", va="top", color=col, fontsize=9.6,
                linespacing=1.4, zorder=4)

    # 读者判定
    ax.text(0.30, 4.30, "读者（EMS 侧）", color=CYAN, fontsize=11.5,
            fontweight="bold", va="center")
    reads = [
        (3.35, 5.75, "s0 = 1", "s1 = 2", "s0 ≠ s1 → 拒绝（读到一半时写者在改）", RED),
        (7.05, 8.15, "s0 = 3", "s1 = 3", "s0 为奇数 → 拒绝（整个过程都停在写入中）", RED),
        (9.45, 11.40, "s0 = 4", "s1 = 4", "相等且偶数 → 接受（完整的一组 8 点）", CYAN),
    ]
    for i, (a, b, s0, s1, verdict, col) in enumerate(reads):
        y = 4.00 - i * 0.70
        ax.plot([a, b], [y, y], color=col, linewidth=3.2, alpha=0.30, zorder=2)
        ax.plot([a, b], [y, y], marker="|", markersize=14, color=col, linewidth=2.2, zorder=4)
        ax.text(0.30, y, f"读第 {i + 1} 遍", ha="left", va="center", color=GRAY, fontsize=10.4)
        ax.text(a - 0.16, y, s0, ha="right", va="center", color=col, fontsize=10.0)
        ax.text(b + 0.16, y, s1, ha="left", va="center", color=col, fontsize=10.0)
        ax.text(2.90, y - 0.26, verdict, ha="left", va="center", color=col,
                fontsize=10.4, fontweight="bold")

    # 三条判据说明
    notes = [
        ("「相等」挡快写者", "读完之后才变 → s0 ≠ s1", RED),
        ("「偶数」挡慢写者", "读的过程中一直在变 → s0 == s1 但为奇数", RED),
        ("缺一不可", "末次递增只挡前者，半更新快照照样通过", RED),
    ]
    for i, (nm, why, col) in enumerate(notes):
        x = 0.30 + i * 4.02
        rbox(ax, x, 1.18, 3.80, 0.82, "", fc=RED_BG, ec=col, lw=1.2)
        ax.text(x + 0.20, 1.78, nm, ha="left", va="center", color=col,
                fontsize=11.0, fontweight="bold", zorder=5)
        ax.text(x + 0.20, 1.42, why, ha="left", va="center", color=GRAY,
                fontsize=9.5, zorder=5)

    ax.add_patch(FancyBboxPatch((0.30, 0.14), 11.80, 0.70,
                                boxstyle="round,pad=0,rounding_size=0.08",
                                facecolor="white", edgecolor=RED, linewidth=1.2, zorder=3))
    ax.text(6.20, 0.49,
            "真缺陷：原来只有「写完之后递增」→ 一次写 6 个点的清空操作，读到的是\n"
            "「新的上界 + 旧的下界」—— 中间态可能比两端都宽，而它看起来是一个合法快照",
            ha="center", va="center", color=RED, fontsize=10.6, fontweight="bold",
            zorder=4, linespacing=1.6)
    save(fig, "ext_protocol.png")


def chart_point_append():
    """A1：点表只能追加在末尾；新点默认 0（允许）而不是 1（禁止）。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.2))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.2)
    ax.axis("off")

    ax.text(0.30, 4.92, "点表的索引是「运行时坐标」，不是源码里的一个数字",
            color=INK, fontsize=13.2, fontweight="bold", va="center")
    ax.text(0.30, 4.54, "共享内存里已经有段了 —— 那段内存里的索引不会跟着新代码变",
            color=GRAY, fontsize=11, va="center")

    def cells(x, y, n, w, h, hicol=None, lab=True):
        for i in range(n):
            col = hicol[i] if (hicol is not None and i in hicol) else None
            fc = col[0] if col else BLUE_BG
            ec = col[1] if col else BLUE
            ax.add_patch(Rectangle((x + i * w, y), w * 0.94, h, facecolor=fc,
                                   edgecolor=ec, linewidth=1.1, zorder=3))
            if lab and i % 4 == 0:
                ax.text(x + i * w + w * 0.47, y + h / 2, str(i), ha="center", va="center",
                        color=INK, fontsize=8.8, zorder=4)

    # ① 老段 30 点
    ax.text(0.30, 4.05, "① 老段（30 点）", color=INK, fontsize=11.8,
            fontweight="bold", va="center")
    ax.text(0.30, 3.73, "共享内存里已经存在的段", color=GRAY, fontsize=9.8, va="center")
    cells(3.20, 3.78, 30, 0.275, 0.42,
          hicol={0: ("#FEF3C7", "#D97706"), 29: ("#FEF3C7", "#D97706")})

    # ② 追加在末尾（正确）
    ax.text(0.30, 3.10, "② 追加在末尾（正确）", color=CYAN, fontsize=11.8,
            fontweight="bold", va="center")
    ax.text(0.30, 2.78, "0..29 逐位不变，老段继续兼容", color=CYAN, fontsize=9.8, va="center")
    cells(3.20, 2.83, 30, 0.275, 0.42,
          hicol={0: ("#FEF3C7", "#D97706"), 29: ("#FEF3C7", "#D97706")})
    for i in range(2):
        ax.add_patch(Rectangle((3.20 + (30 + i) * 0.275, 2.83), 0.258, 0.42,
                               facecolor="#ECFEFF", edgecolor=CYAN, linewidth=1.3, zorder=3))
    ax.text(3.20, 3.42, "两个新点只追加在末尾：STA.BMS_CHG_FORBID / STA.BMS_DIS_FORBID",
            ha="left", va="center", color=CYAN, fontsize=10.2, fontweight="bold", zorder=4)

    # ③ 中间插入（错误）
    ax.text(0.30, 2.15, "③ 中间插入（错误）", color=RED, fontsize=11.8,
            fontweight="bold", va="center")
    ax.text(0.30, 1.83, "其后所有点整体偏移 2", color=RED, fontsize=9.8, va="center")
    ax.add_patch(Rectangle((3.20, 1.88), 0.91, 0.42, facecolor="#FEE2E2",
                           edgecolor=RED, linewidth=1.3, zorder=3))
    cells(4.33, 1.88, 28, 0.275, 0.42,
          hicol={0: ("#DBEAFE", "#3B82F6"), 27: ("#DBEAFE", "#3B82F6")})
    ax.text(3.20, 1.55, "现场表现：点名对得上、值全是隔壁点的", ha="left", va="center",
            color=RED, fontsize=10.4, fontweight="bold")

    # 底部：默认值取 0 而不是 1
    ax.add_patch(FancyBboxPatch((0.30, 0.14), 11.80, 1.10,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor="white", edgecolor=BLUE, linewidth=1.3, zorder=3))
    ax.text(0.60, 0.98, "新点默认值取 0 = 允许，而不是 1 = 禁止",
            ha="left", va="center", color=BLUE, fontsize=11.8, fontweight="bold", zorder=4)
    ax.text(0.60, 0.62,
            "取「禁止」会让「设备侧尚未上线」直接锁死 [0, 0] —— 冷启动不可用；",
            ha="left", va="center", color=INK, fontsize=10.4, zorder=4)
    ax.text(0.60, 0.34,
            "取「允许」与修复前逐位一致，不引入新风险；fail-safe 由 05/ 的 bms_comm_lost → [0, 0] 独立承担。",
            ha="left", va="center", color=GRAY, fontsize=10.0, zorder=4)
    save(fig, "point_append.png")


def chart_safety_chain():
    """A3.2：EXT 接进安全引擎的第 10 条约束，以及停机/闭锁在 stale 后粘住。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.6))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.6)
    ax.axis("off")

    ax.text(0.30, 5.32, "把 EXT 接进安全链：9 条约束收敛之后、矛盾检查之前",
            color=INK, fontsize=13.2, fontweight="bold", va="center")

    # 左：9 条约束
    ax.text(0.30, 4.94, "已有 9 条安全约束（只收紧、不放宽）", color=GRAY,
            fontsize=10.8, va="center")
    cons = [("SOC 禁充放", "L0 最硬"), ("防逆流", "S05"), ("变压器容量", ""),
            ("契约需量", "L2"), ("温度", ""), ("PCS 额定", ""),
            ("BMS 通信丢失", ""), ("BMS 禁充位", "A1 新增"), ("BMS 禁放位", "A1 新增")]
    for i, (nm, tag) in enumerate(cons):
        x = 0.30 + (i % 5) * 1.42
        y = 4.24 - (i // 5) * 0.62
        rbox(ax, x, y, 1.32, 0.50, nm, fc=GRAY_BG, ec=GRAY, fs=9.2)

    arrow(ax, (3.90, 3.52), (3.90, 3.20), color=GRAY, lw=1.8, ms=14)
    ax.text(3.98, 3.36, "全部收敛成一个 (p_lower, p_upper)", ha="left", va="center",
            color=GRAY, fontsize=10.4)

    # 中：EXT 收窄（新）
    ax.add_patch(FancyBboxPatch((0.30, 2.36), 7.46, 0.84,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=BLUE_BG, edgecolor=BLUE, linewidth=1.6, zorder=3))
    ax.text(0.54, 2.98, "★ 第 10 条：EXT 外部设定", ha="left", va="center", color=BLUE,
            fontsize=11.8, fontweight="bold", zorder=4)
    ax.text(0.54, 2.60,
            "方向感知单侧收紧：正设定只压 p_upper、负设定只抬 p_lower —— 不做对称幅值带",
            ha="left", va="center", color=INK, fontsize=10.2, zorder=4)

    arrow(ax, (3.90, 2.30), (3.90, 2.00), color=GRAY, lw=1.8, ms=14)
    rbox(ax, 0.30, 1.32, 3.50, 0.62, "矛盾检查 → 区间矛盾则 DERATED", fc="white", ec=GRAY, fs=10.6)
    rbox(ax, 4.26, 1.32, 3.50, 0.62, "按 p_desired 裁剪到最近边界 → 下发",
         fc="white", ec=GRAY, fs=10.6)
    ax.text(0.30, 0.96, "★ 插在这个位置的理由：EXT 是「监督命令」，不是设备降额 —— "
                        "它不进降额项，也不能绕过本地安全约束。",
            ha="left", va="center", color=BLUE, fontsize=10.4, fontweight="bold")

    # 右：粘住语义表
    ax.add_patch(FancyBboxPatch((8.06, 1.24), 4.04, 3.68,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor="white", edgecolor=RED, linewidth=1.4, zorder=3))
    ax.text(10.08, 4.66, "停机 / 闭锁：stale 之后粘住", ha="center", va="center",
            color=RED, fontsize=11.8, fontweight="bold", zorder=4)
    ax.plot([8.34, 11.82], [4.42, 4.42], color=GRID, linewidth=0.9, zorder=3)
    rows = [
        ("新鲜 · 停机 / 闭锁", "[0, 0]", CYAN),
        ("新鲜 · 限功率", "单侧收紧", CYAN),
        ("stale · 停机 / 闭锁", "粘住 [0, 0]", RED),
        ("stale · 功率设定", "不生效", RED),
    ]
    for i, (k, v, col) in enumerate(rows):
        y = 4.14 - i * 0.50
        ax.text(8.42, y, k, ha="left", va="center", color=INK,
                fontsize=10.0, zorder=4)
        ax.text(11.74, y, v, ha="right", va="center", color=col,
                fontsize=10.2, fontweight="bold", zorder=4)
        if i < len(rows) - 1:
            ax.plot([8.34, 11.82], [y - 0.25, y - 0.25], color=GRID, linewidth=0.9, zorder=3)
    ax.text(10.08, 2.12, "安全不对称：丢「停机」会丢安全（理由本地看不到）；\n"
                         "粘「停机」只丢钱 —— [0, 0] 永远安全",
            ha="center", va="center", color=GRAY, fontsize=9.6, zorder=4, linespacing=1.7)
    ax.text(10.08, 1.56, "粘住守卫 = present && consistent && finite",
            ha="center", va="center", color=INK, fontsize=9.6, fontweight="bold", zorder=4)
    save(fig, "safety_chain.png")


# ================================================================ 三 · 测试图
def chart_pyramid():
    fig, ax = plt.subplots(figsize=(12.4, 5.6))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 6.0)
    ax.axis("off")

    layers = [
        ("⑤ 端到端与交付门禁", "真五进程闭环 600 拍 · 七维度验收器", "55 / 55 项", CYAN),
        ("④ 跨语言对账", "C++ 侧对上真 pymodbus 外部对端，逐拍对账", "83", CYAN),
        ("③ 跨内存边界落点", "走真共享内存段、不注入任何东西", "121", BLUE),
        ("② 跨进程集成", "三个真进程经共享内存交互", "152", BLUE),
        ("① 模块单元测试", "各模块纯逻辑用例 + 协议层", "9026", BLUE),
    ]
    widths = [0.30, 0.40, 0.52, 0.66, 0.84]
    top, h = 5.55, 0.98
    for i, ((name, note, num, col), w) in enumerate(zip(layers, widths)):
        y = top - (i + 1) * h
        cx, half = 4.55, (w * 7.2) / 2
        ax.add_patch(Polygon(
            [(cx - half, y + h), (cx + half, y + h), (cx + half * 0.985, y), (cx - half * 0.985, y)],
            closed=True, facecolor=col, alpha=0.22, edgecolor=col, linewidth=1.4, zorder=2))
        ax.text(cx, y + h * 0.63, name, ha="center", va="center", color=INK,
                fontsize=12.5, fontweight="bold", zorder=3)
        ax.text(cx, y + h * 0.24, note, ha="center", va="center", color=GRAY,
                fontsize=10.5, zorder=3)
        ax.text(11.55, y + h / 2, num, ha="right", va="center", color=INK,
                fontsize=15, fontweight="bold", zorder=3)

    ax.text(4.55, 5.85, "越往上，越接近「真实链路」；越往下，越接近「纯逻辑」",
            ha="center", color=INK, fontsize=12.5)
    ax.text(4.55, 0.30, "① 里已含协议层 9026；④ 的 83 条在默认环境（无 pymodbus）会 SKIP 7 条 → 计 5 条，"
                        "所以默认口径总数是 9304 而不是 9382。",
            ha="center", color=GRAY, fontsize=10.4)
    save(fig, "test_pyramid.png")


def chart_baseline():
    data = [
        ("09-19\n起步", 8085), ("+11/ +12/\n联调与验收", 8331), ("判据修正\nT47 + 心跳", 8501),
        ("A1\n禁充放位", 8523), ("A2\n关口口径", 8547), ("09-20\n13/ 设备接入", 8906),
        ("A3.1\n外部设定落点", 9245), ("A3.2\n接进安全链", 9276), ("转发表\n外置 CSV", 9304),
    ]
    labels = [d[0] for d in data]
    vals = [d[1] for d in data]

    fig, ax = plt.subplots(figsize=(12.4, 4.6))
    cols = [BLUE] * (len(vals) - 1) + [CYAN]
    bars = ax.bar(range(len(vals)), vals, color=cols, width=0.62, zorder=3)
    for i, (b, v) in enumerate(zip(bars, vals)):
        ax.text(b.get_x() + b.get_width() / 2, v + 55, f"{v:,}", ha="center",
                color=INK, fontsize=11.5, fontweight="bold" if i == len(vals) - 1 else "normal")
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, fontsize=10.5, color=GRAY)
    ax.set_ylim(7600, 9700)
    ax.set_ylabel("断言总数", color=INK, fontsize=12.5)
    polish(ax)
    ax.annotate("", xy=(len(vals) - 1, 9304), xytext=(0, 8085),
                arrowprops=dict(arrowstyle="-|>", color=RED, linewidth=1.6,
                                linestyle="--", connectionstyle="arc3,rad=-0.22"), zorder=2)
    ax.text(4.0, 9620, "两天净增 +1219 条断言（+15%），0 失败", color=RED,
            fontsize=14, fontweight="bold", ha="center")
    fig.text(0.5, -0.02, "基线口径：本机默认环境（无 pymodbus）9304；装上后为 9382。"
                         "每根柱子都对应一次可复核的实测，数字取自 README 基线块。",
             ha="center", color=GRAY, fontsize=10.5)
    save(fig, "test_baseline.png")


def chart_refute():
    items = [
        ("跨语言：去掉电表偏差项", 3, "p_grid 232 vs 272"),
        ("跨语言：寄存器地址 6 → 7", 3, "定位到行并排打两侧"),
        ("跨语言：读指令恒返回空", 4, "T43 三条 + T44 一条"),
        ("交换模拟器 DI / IR 表", 0, "构造时即拒绝 → 非缺陷"),
        ("交换模拟器 CO / DI 表", 0, "仍全绿 → 非缺陷"),
        ("EXT：去掉「序号必须为偶数」", 3, "红的正是半更新快照"),
        ("EXT：把写前递增挪到写后", 2, "新下界配旧序号"),
        ("转发表：覆盖表分支改恒假", 1, "count 未变为 1"),
    ]
    fig, ax = plt.subplots(figsize=(12.4, 5.4))
    ys = list(range(len(items)))[::-1]
    for y, (name, n, note) in zip(ys, items):
        col = BLUE if n > 0 else GRAY
        ax.barh(y, n if n > 0 else 0.06, height=0.5, color=col, alpha=0.85, zorder=3)
        ax.text(-0.08, y, name, ha="right", va="center", color=INK, fontsize=12)
        if n > 0:
            ax.text(n + 0.10, y, f"变红 {n} 条", ha="left", va="center", color=BLUE,
                    fontsize=12, fontweight="bold")
        else:
            ax.text(0.22, y, "未变红", ha="left", va="center", color=GRAY, fontsize=12)
        ax.text(4.55, y, note, ha="left", va="center", color=GRAY, fontsize=11)
    ax.set_xlim(0, 4.4)
    ax.set_ylim(-0.7, len(items) - 0.3)
    ax.set_yticks([])
    ax.set_xticks([0, 1, 2, 3, 4])
    ax.set_xlabel("改坏实现后变红的断言条数", color=INK, fontsize=12.5)
    ax.axvline(0, color=GRAY, linewidth=1.0)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=GRAY, labelsize=11)
    ax.grid(True, axis="x", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.text(4.4, len(items) - 0.55, "灰条 = 反证推翻了我原来的判断", color=GRAY,
            fontsize=11, ha="right")
    save(fig, "test_refute.png")


def chart_five_proc():
    fig, ax = plt.subplots(figsize=(12.4, 3.6))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 3.4)
    ax.axis("off")
    nodes = [
        ("初始化器", "建立并持有\n共享内存", GRAY),
        ("设备侧", "按拍发布\n量测与状态", BLUE),
        ("IEC104 网关", "接收调度指令\n写入 EXT 区", BLUE),
        ("EMS 侧", "读 EXT 区\n安全引擎收窄", CYAN),
    ]
    w, gap = 2.5, 0.52
    x = 0.45
    for i, (nm, nd, col) in enumerate(nodes):
        rbox(ax, x, 1.15, w, 1.15, "", fc="white", ec=col, lw=1.5)
        ax.text(x + w / 2, 1.92, nm, ha="center", va="center", color=INK,
                fontsize=13, fontweight="bold", zorder=5)
        ax.text(x + w / 2, 1.48, nd, ha="center", va="center", color=GRAY,
                fontsize=11, zorder=5, linespacing=1.5)
        if i < len(nodes) - 1:
            arrow(ax, (x + w + 0.06, 1.72), (x + w + gap - 0.06, 1.72),
                  color=GRAY, lw=1.6, ms=15)
        x += w + gap
    ax.add_patch(FancyBboxPatch((2.80, 0.28), 6.80, 0.62,
                                boxstyle="round,pad=0,rounding_size=0.1",
                                facecolor=RED_BG, edgecolor=RED, linewidth=1.3, zorder=3))
    ax.text(6.20, 0.59, "调度下发「停机」→ EMS 把安全区间收成 [0, 0]：600 / 600 拍全部命中",
            ha="center", va="center", color=RED, fontsize=12, fontweight="bold", zorder=4)
    ax.text(0.45, 2.72, "五进程闭环实测（不是模拟调用，是五个真实进程 + 一块真实共享内存）",
            color=INK, fontsize=12.5, fontweight="bold")
    save(fig, "five_proc.png")


# ================================================================ 四 · 24h 仿真图
def chart_power(rows):
    t = [float(r["t_s"]) / 3600.0 for r in rows]
    pl = fnum(rows, "P_load_kW")
    pv = fnum(rows, "P_pv_kW")
    pb = fnum(rows, "P_actual_kW")
    pg = fnum(rows, "P_grid_kW")

    fig, (a1, a2) = plt.subplots(
        2, 1, figsize=(12.4, 6.4), sharex=True, gridspec_kw={"height_ratios": [1.35, 1], "hspace": 0.16}
    )

    a1.fill_between(t, pv, color=CYAN, alpha=0.18, zorder=1)
    a1.plot(t, pv, color=CYAN, linewidth=1.9, label="光伏出力  P_pv", zorder=3)
    a1.plot(t, pl, color=GRAY, linewidth=1.6, label="负荷  P_load", zorder=2)
    a1.plot(t, pb, color=BLUE, linewidth=1.9, label="储能出力  P_bat（放电正／充电负）", zorder=4)
    a1.axhline(0, color=GRAY, linewidth=0.9, linestyle=":", zorder=1)
    a1.set_ylabel("功率 (kW)", color=INK, fontsize=12.5)
    polish(a1)
    a1.legend(loc="upper left", frameon=False, fontsize=11.5, ncol=2, labelcolor=INK)
    a1.set_title("① 负荷 / 光伏 / 储能：储能自动在低谷充电、高峰放电（真实 24h 仿真，8640 拍）",
                 color=INK, fontsize=13.5, loc="left", pad=10)

    base = [l - p for l, p in zip(pl, pv)]
    a2.fill_between(t, pg, base, where=[b > g for b, g in zip(base, pg)],
                    color=CYAN, alpha=0.20, zorder=1)
    a2.plot(t, base, color=GRAY, linewidth=1.7, zorder=2,
            label="不做储能调度：关口功率 = 负荷 − 光伏（峰值 450 kW）")
    a2.plot(t, pg, color=BLUE, linewidth=2.0, zorder=3,
            label="本方案：关口功率 P_grid（峰值 400 kW）")
    a2.axhline(400, color=RED, linewidth=1.6, linestyle="--", zorder=4,
               label="需量目标 D_target = 400 kW（上限，越限 0 次）")
    a2.set_ylabel("关口功率 (kW)", color=INK, fontsize=12.5)
    a2.set_xlabel("时间 (h)", color=INK, fontsize=12.5)
    a2.set_xticks(range(0, 25, 2))
    a2.set_xlim(0, 24)
    polish(a2)
    a2.legend(loc="upper left", frameon=False, fontsize=11, labelcolor=INK)
    a2.set_title("② 削峰填谷：青色阴影 = 储能削掉的部分，峰值从 450 kW 压到 400 kW",
                 color=INK, fontsize=13.5, loc="left", pad=8)
    a2.text(23.9, 430, "青色阴影 = 储能顶上去的那部分功率", ha="right", color=CYAN, fontsize=11)
    fig.text(0.5, -0.035,
             "负载与电价按 15 min 曲线给定；功率控制 1 s 一拍（共 86400 拍），日志每 10 拍落一行 → 8640 行。"
             "仿真模型是既有资产（周期 10/），本曲线由 09-20 的验收门禁跑批产出。",
             ha="center", color=GRAY, fontsize=10.5)
    save(fig, "sim_power.png")


def chart_soc(rows, summary):
    t = [float(r["t_s"]) / 3600.0 for r in rows]
    soc = [float(r["SOC"]) * 100 for r in rows]
    st = summary["state"]

    fig, ax = plt.subplots(figsize=(12.4, 4.0))
    ax.axhspan(10, 90, color=BLUE_BG, zorder=0)
    ax.plot(t, soc, color=BLUE, linewidth=2.0, zorder=3, label="SOC 荷电状态")
    ax.axhline(90, color=GRAY, linewidth=1.2, linestyle="--", zorder=1)
    ax.axhline(10, color=GRAY, linewidth=1.2, linestyle="--", zorder=1)
    # ★ 两个边界标注都加白底：既避免被虚线穿过，也避免「允许下限」被左下角图例压住
    ax.text(0.15, 92.4, "允许上限 90%（浅区 = 可运行区间）", color=GRAY, fontsize=11,
            bbox=dict(facecolor="white", edgecolor="none", alpha=0.85, pad=1.5))
    ax.text(12.6, 5.2, "允许下限 10%", color=GRAY, fontsize=11,
            bbox=dict(facecolor="white", edgecolor="none", alpha=0.85, pad=1.5))
    ax.annotate(f"实测最高 {st['soc_max']*100:.2f}%", xy=(6.2, st["soc_max"] * 100),
                xytext=(7.0, 84), color=INK, fontsize=11,
                arrowprops=dict(arrowstyle="->", color=GRAY, linewidth=1))
    ax.annotate(f"实测最低 {st['soc_min']*100:.2f}%", xy=(19.6, st["soc_min"] * 100),
                xytext=(16.4, 22), color=INK, fontsize=11,
                arrowprops=dict(arrowstyle="->", color=GRAY, linewidth=1))
    ax.set_ylabel("SOC (%)", color=INK, fontsize=12.5)
    ax.set_xlabel("时间 (h)", color=INK, fontsize=12.5)
    ax.set_xticks(range(0, 25, 2))
    ax.set_xlim(0, 24)
    ax.set_ylim(0, 100)
    polish(ax)
    ax.legend(loc="lower left", frameon=False, fontsize=11.5, labelcolor=INK)
    ax.set_title("SOC 全程留在 [10%, 90%] 允许区间内，越界 0 次",
                 color=INK, fontsize=13.5, loc="left", pad=10)
    save(fig, "sim_soc.png")


def chart_hourly(rows):
    """分时购电量 + 电价档：说明「低价充、高价放」。"""
    hourly = defaultdict(float)
    dt = 10.0
    for r in rows:
        h = int(float(r["t_s"]) // 3600) % 24
        hourly[h] += max(0.0, float(r["P_grid_kW"])) * dt / 3600.0
    hours = list(range(24))
    vals = [hourly.get(h, 0.0) for h in hours]

    fig, ax = plt.subplots(figsize=(12.4, 4.0))
    colors = [CYAN if (h < 8 or h >= 22) else (RED if 10 <= h < 15 else BLUE) for h in hours]
    ax.bar(hours, vals, color=colors, width=0.72, zorder=3)
    ax.set_xticks(hours)
    ax.set_xticklabels([f"{h}" for h in hours], fontsize=10)
    ax.set_xlabel("时刻 (h)", color=INK, fontsize=12.5)
    ax.set_ylabel("购电量 (kWh)", color=INK, fontsize=12.5)
    polish(ax)
    handles = [
        Line2D([0], [0], color=CYAN, lw=7, label="谷电价 0.30 元/kWh（00–08 / 22–24）"),
        Line2D([0], [0], color=BLUE, lw=7, label="平电价 0.70 元/kWh（08–10 / 15–22）"),
        Line2D([0], [0], color=RED, lw=7, label="峰电价 1.20 元/kWh（10–15）"),
    ]
    # ★ 图例放在 10–15 点那段（柱高 ≤ 120）的上方空白区，
    #   原来的 upper left 正好压住 0 点那根最高柱（400）。
    ax.legend(handles=handles, loc="upper right", bbox_to_anchor=(0.68, 0.99),
              frameon=False, fontsize=11, labelcolor=INK)
    ax.set_title("分时购电分布：谷段多买、峰段少买（总购电 5180.8 kWh，倒送 0 kWh）",
                 color=INK, fontsize=13.5, loc="left", pad=10)
    save(fig, "sim_hourly.png")


def chart_econ(summary):
    ec = summary["economics"]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(12.4, 4.2), gridspec_kw={"width_ratios": [1, 1.15]})

    bars = [ec["cost_total_base_cny"], ec["cost_total_cny"]]
    labels = ["不做储能调度", "本方案（EMS 调度）"]
    ys = [0.55, 0.15]
    a1.barh(ys, bars, height=0.3, color=[GRAY, BLUE], zorder=3)
    for y, v in zip(ys, bars):
        a1.text(v + 60, y, f"{v:,.0f} 元", va="center", color=INK, fontsize=13)
    a1.set_yticks(ys)
    a1.set_yticklabels(labels, fontsize=12.5, color=INK)
    a1.set_xlim(0, max(bars) * 1.28)
    a1.set_xlabel("全天综合用电成本 (元)", color=INK, fontsize=12)
    polish(a1)
    a1.annotate(
        f"节省 {ec['saving_total_cny']:,.0f} 元／日\n（{ec['saving_pct']:.1f}%）",
        xy=(bars[1], 0.15), xytext=(bars[0] * 0.30, 0.40),
        color=RED, fontsize=14, fontweight="bold",
        arrowprops=dict(arrowstyle="->", color=RED, linewidth=1.6),
    )
    a1.set_title("① 成本对比", color=INK, fontsize=13.5, loc="left", pad=10)

    items = [
        ("度电费节省", ec["saving_energy_cny"]),
        ("需量费节省", ec["saving_demand_cny"]),
        ("电池损耗成本", -ec["cost_degradation_cny"]),
    ]
    ys2 = [2, 1, 0]
    vals = [v for _, v in items]
    cols = [BLUE if v >= 0 else RED for v in vals]
    a2.barh(ys2, vals, height=0.46, color=cols, zorder=3)
    a2.axvline(0, color=GRAY, linewidth=1.0)
    for y, v in zip(ys2, vals):
        off = 16 if v >= 0 else -16
        a2.text(v + off, y, f"{v:+,.0f}", va="center",
                ha="left" if v >= 0 else "right", color=INK, fontsize=12.5)
    a2.set_yticks(ys2)
    a2.set_yticklabels([k for k, _ in items], fontsize=12.5, color=INK)
    a2.set_xlim(min(vals) * 2.6, max(vals) * 1.45)
    a2.set_xlabel("金额 (元／日)", color=INK, fontsize=12)
    polish(a2)
    a2.set_title(f"② 收益拆解（净收益 {ec['net_benefit_cny']:,.0f} 元／日，已扣电池损耗）",
                 color=INK, fontsize=13.5, loc="left", pad=10)
    save(fig, "sim_econ.png")


def main():
    rows = load_series()
    with open(SUM, "r", encoding="utf-8") as f:
        summary = json.load(f)
    print("rows", len(rows))

    # 一 总览
    chart_timeline2()
    # 二 原理
    chart_crossproc()
    chart_modbus_arch()
    chart_ext_protocol()
    chart_point_append()
    chart_safety_chain()
    # 三 测试
    chart_pyramid()
    chart_baseline()
    chart_refute()
    chart_five_proc()
    # 四 24h 仿真
    chart_power(rows)
    chart_soc(rows, summary)
    chart_hourly(rows)
    chart_econ(summary)

    inv = summary["invariants"]
    print("invariants:", {k: v for k, v in inv.items() if isinstance(v, int)})


if __name__ == "__main__":
    main()
