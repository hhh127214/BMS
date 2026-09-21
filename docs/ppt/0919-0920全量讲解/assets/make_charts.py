# -*- coding: utf-8 -*-
"""生成「0919-0920 全量讲解」PPT 所需的 5 张图表。

★ 硬约束：图内标签一律用功能名（设备接入 / 调度网关 / 安全引擎…），
  绝不出现内部模块号 / 文件名 / 缺口编号（06、A3.2、T47 之类）。
  其中 modbus_arch / crossproc / baseline 三张是既有图的「去文件名清洗版」：
  坐标模板与配色复用 交付汇报/assets/make_charts.py，数据逐位不变，只改标签。

运行：C:/Users/17128/.workbuddy/binaries/python/versions/3.13.12/python.exe make_charts.py
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

BLUE = "#3B82F6"
CYAN = "#06B6D4"
RED = "#EF4444"
GRAY = "#94A3B8"
INK = "#1A1A1A"
GRID = "#E2E8F0"
BLUE_BG = "#EFF6FF"
CYAN_BG = "#ECFEFF"
RED_BG = "#FEF2F2"
GRAY_BG = "#F8FAFC"

ASSETS = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------- 基础设施
def save(fig, name):
    p = os.path.join(ASSETS, name)
    fig.savefig(p, dpi=150, facecolor="white", bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)
    print("wrote", name)


def rbox(ax, x, y, w, h, text, fc="white", ec=GRAY, tc=INK, fs=12,
         bold=False, lw=1.4, radius=0.12, zorder=3, linespacing=1.5):
    ax.add_patch(FancyBboxPatch(
        (x, y), w, h, boxstyle=f"round,pad=0,rounding_size={radius}",
        linewidth=lw, facecolor=fc, edgecolor=ec, zorder=zorder))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            color=tc, fontsize=fs, zorder=zorder + 1,
            fontweight="bold" if bold else "normal", linespacing=linespacing)


def arrow(ax, p1, p2, color=GRAY, style="-|>", lw=1.7, ls="solid", rad=0.0, zorder=2, ms=15):
    ax.add_patch(FancyArrowPatch(
        p1, p2, arrowstyle=style, mutation_scale=ms, linewidth=lw, color=color,
        linestyle=ls, zorder=zorder, connectionstyle=f"arc3,rad={rad}"))


# ================================================================ 01 落点总图（新做）
def chart_landing_map():
    """三线一总线：设备线 / 调度线 / 内部线全部汇进 40 点实时数据总线。"""
    fig, ax = plt.subplots(figsize=(12.4, 5.8))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 5.8)
    ax.axis("off")

    ax.text(0.30, 5.55, "两天落点总图：三条线，全部汇进同一条实时数据总线",
            color=INK, fontsize=13.5, fontweight="bold", va="center")
    ax.text(0.30, 5.18, "设备线（对下接设备）· 调度线（对上接调度）· 内部线（总线扩容与安全链接线）",
            color=GRAY, fontsize=11, va="center")

    lane_y, lane_h = 4.10, 0.92

    # ---- 设备线（左上，青）
    rbox(ax, 0.30, lane_y + 0.26, 0.92, 0.40, "设备线", fc=CYAN_BG, ec=CYAN, tc=CYAN,
         fs=10.5, bold=True, radius=0.08)
    rbox(ax, 1.42, lane_y, 2.46, lane_h, "", fc=GRAY_BG, ec=GRAY, lw=1.3)
    ax.text(2.65, lane_y + 0.60, "设备模拟器（测试替身）", ha="center", color=INK,
            fontsize=10.6, fontweight="bold", zorder=5)
    ax.text(2.65, lane_y + 0.25, "仿真 BMS / PCS / 电表 · 32 点寄存器", ha="center",
            color=GRAY, fontsize=8.6, zorder=5)
    arrow(ax, (3.94, lane_y + lane_h / 2), (4.32, lane_y + lane_h / 2), color=GRAY, lw=1.3, ms=12)
    rbox(ax, 4.34, lane_y, 2.66, lane_h, "", fc=CYAN_BG, ec=CYAN, lw=1.5)
    ax.text(5.67, lane_y + 0.60, "设备接入（新建）", ha="center", color=INK,
            fontsize=10.8, fontweight="bold", zorder=5)
    ax.text(5.67, lane_y + 0.25, "Modbus TCP 主站 · 设备通道适配器", ha="center",
            color=GRAY, fontsize=8.6, zorder=5)

    # 设备接入 ↔ 总线（指令下行 / 量测上行）
    arrow(ax, (5.00, lane_y - 0.02), (5.00, 3.46), color=BLUE, lw=1.6, ms=13)
    arrow(ax, (6.30, 3.46), (6.30, lane_y - 0.02), color=CYAN, lw=1.6, ms=13)
    ax.text(4.90, 3.78, "指令下行", ha="right", color=BLUE, fontsize=8.8, fontweight="bold")
    ax.text(6.42, 3.78, "量测/状态上行", ha="left", color=CYAN, fontsize=8.8, fontweight="bold")

    # ---- 调度线（右上，蓝）
    rbox(ax, 7.30, lane_y + 0.26, 0.92, 0.40, "调度线", fc=BLUE_BG, ec=BLUE, tc=BLUE,
         fs=10.5, bold=True, radius=0.08)
    rbox(ax, 8.42, lane_y, 2.10, lane_h, "", fc=GRAY_BG, ec=GRAY, lw=1.3)
    ax.text(9.47, lane_y + 0.60, "调度主站模拟器", ha="center", color=INK,
            fontsize=10.6, fontweight="bold", zorder=5)
    ax.text(9.47, lane_y + 0.25, "（测试替身）", ha="center", color=GRAY, fontsize=8.6, zorder=5)
    arrow(ax, (10.58, lane_y + lane_h / 2), (10.96, lane_y + lane_h / 2), color=GRAY, lw=1.3, ms=12)
    rbox(ax, 10.98, lane_y, 1.42, lane_h, "", fc=BLUE_BG, ec=BLUE, lw=1.5)
    ax.text(11.69, lane_y + 0.60, "调度网关（新建）", ha="center", color=INK,
            fontsize=9.8, fontweight="bold", zorder=5)
    ax.text(11.69, lane_y + 0.25, "IEC 104 从站", ha="center", color=GRAY, fontsize=8.6, zorder=5)

    # 网关 ↔ 总线
    arrow(ax, (11.18, lane_y - 0.02), (11.18, 3.46), color=BLUE, lw=1.6, ms=13)
    arrow(ax, (11.88, 3.46), (11.88, lane_y - 0.02), color=CYAN, lw=1.6, ms=13)
    ax.text(11.08, 3.78, "遥控遥调下行", ha="right", color=BLUE, fontsize=8.2, fontweight="bold")
    ax.text(11.98, 3.78, "遥测上送", ha="left", color=CYAN, fontsize=8.2, fontweight="bold")

    # ---- 中央总线（40 点五分区）
    rbox(ax, 0.30, 2.26, 11.80, 1.18, "", fc="white", ec=INK, lw=1.6, radius=0.10)
    ax.text(0.55, 3.24, "实时数据总线 · 40 点（共享内存，唯一交互通道）", ha="left", color=INK,
            fontsize=10.8, fontweight="bold", zorder=5)
    segs = [
        ("量测 7", 1.94, BLUE_BG, BLUE, INK),
        ("状态 8\n含禁充/禁放 2", 2.22, BLUE_BG, BLUE, INK),
        ("配置 14", 3.88, BLUE_BG, BLUE, INK),
        ("指令 3", 0.83, BLUE_BG, BLUE, INK),
        ("外来指令 8\n本次新增", 2.22, RED_BG, RED, RED),
    ]
    x = 0.50
    for nm, w, fc, ec, tc in segs:
        rbox(ax, x, 2.60, w, 0.52, nm, fc=fc, ec=ec, tc=tc, fs=8.8, lw=1.1,
             radius=0.06, linespacing=1.3)
        x += w + 0.08
    ax.text(0.55, 2.42,
            "三个写入者各写一段、互不重叠（设备侧 / EMS / 网关）｜外来设定失效双判据："
            "断开即清空（快）+ 时标超期（兜底）",
            ha="left", color=GRAY, fontsize=9.2, va="center", zorder=5)

    # ---- 内部线：总线 ↔ EMS（青=读，蓝=写）
    arrow(ax, (5.60, 2.24), (5.60, 1.76), color=CYAN, lw=1.6, ms=13)
    arrow(ax, (6.90, 1.76), (6.90, 2.24), color=BLUE, lw=1.6, ms=13)
    ax.text(5.48, 2.00, "读量测 · 外来设定", ha="right", color=CYAN, fontsize=8.8, fontweight="bold")
    ax.text(7.02, 2.00, "写指令区", ha="left", color=BLUE, fontsize=8.8, fontweight="bold")
    rbox(ax, 8.30, 1.80, 0.92, 0.40, "内部线", fc=GRAY_BG, ec=GRAY, tc=INK,
         fs=10.5, bold=True, radius=0.08)
    ax.text(9.36, 2.00, "外来指令区落点 → 安全引擎收窄", ha="left", color=GRAY, fontsize=9.4, va="center")

    # ---- EMS 核心
    rbox(ax, 2.20, 0.88, 8.00, 0.84, "", fc=BLUE_BG, ec=BLUE, lw=1.6, radius=0.10)
    ax.text(6.20, 1.46, "EMS 核心 · 安全引擎", ha="center", color=INK,
            fontsize=11.5, fontweight="bold", zorder=5)
    ax.text(6.20, 1.08, "10 条安全约束收敛成一个 (p_lower, p_upper) → 策略按期望值裁剪 → 指令区下发",
            ha="center", color=GRAY, fontsize=9.4, zorder=5)

    # ---- 底部横幅
    ax.add_patch(FancyBboxPatch((0.30, 0.10), 11.80, 0.55,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=RED_BG, edgecolor=RED, linewidth=1.3, zorder=3))
    ax.text(6.20, 0.375,
            "两条新通路 + 一次安全链接线：调度指令落得到地方、现场设备接得进来、"
            "外来设定进得了安全链 —— 每一步都有断言守着",
            ha="center", va="center", color=RED, fontsize=11.2, fontweight="bold", zorder=4)
    save(fig, "landing_map.png")


# ================================================================ 02 模拟器架构（清洗版）
def chart_modbus_arch():
    """对下接现场设备：Modbus TCP 主从架构与接口方向。★ 去文件名版。"""
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
    ax.text(8.03, y + h * 0.72, "设备接入适配器", ha="center", va="center", color=INK,
            fontsize=11.8, fontweight="bold", zorder=5)
    ax.text(8.03, y + h * 0.30, "设备通道第 4 个实现\n与仿真适配器可互换",
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


# ================================================================ 03 跨进程联调（清洗版）
def chart_crossproc():
    """为什么必须跨进程：单进程 + 夹具注入 = 测试盲区。★ 去文件名版。"""
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
    for i, nm in enumerate(["调度算法", "安全约束", "EMS 主循环"]):
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
    ax.text(9.30, 4.68, "跨进程（新建联调）", ha="center", color=BLUE,
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
        ("①", "主循环从未把指令写进指令区",
         "权限区间恒 [0, 0] → 3000 拍里 2978 拍被判「指令越界」"),
        ("②", "并发碰撞被误计成「采集失败」",
         "三进程下 600 拍出现 51 次假告警（语义应是「请重试」）"),
        ("③", "内存设备通道缺热模型",
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


# ================================================================ 04 基线演进（清洗版）
def chart_baseline():
    """断言基线九连涨。★ 标签全功能化，数据与原图逐位一致。"""
    data = [
        ("起步", 8085), ("跨进程联调\n+验收门禁", 8331), ("并发判据修正\n+心跳换算", 8501),
        ("禁充放位\n接线", 8523), ("关口功率\n只读电表", 8547), ("设备接入\n新模块", 8906),
        ("外部设定\n落点", 9245), ("落点接进\n安全链", 9276), ("转发表\n外置", 9304),
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
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=GRAY, labelsize=11)
    ax.grid(True, axis="y", color=GRID, linewidth=0.8, alpha=0.9)
    ax.set_axisbelow(True)
    ax.annotate("", xy=(len(vals) - 1, 9304), xytext=(0, 8085),
                arrowprops=dict(arrowstyle="-|>", color=RED, linewidth=1.6,
                                linestyle="--", connectionstyle="arc3,rad=-0.22"), zorder=2)
    ax.text(4.0, 9620, "两天净增 +1219 条断言（+15%），0 失败", color=RED,
            fontsize=14, fontweight="bold", ha="center")
    fig.text(0.5, -0.02, "基线口径：本机默认环境（无 pymodbus）9304；装上后为 9382。"
                         "每根柱子都对应一次可复核的实测。",
             ha="center", color=GRAY, fontsize=10.5)
    save(fig, "baseline.png")


# ================================================================ 05 模拟器测试结果（新做）
def chart_sim_tests():
    """设备模拟器三层测试结果 + 活体演示 + 口径复现。"""
    fig, ax = plt.subplots(figsize=(12.4, 4.3))
    ax.set_xlim(0, 12.4)
    ax.set_ylim(0, 4.3)
    ax.axis("off")

    ax.text(0.30, 4.05, "设备模拟器测试结果：3 层 · 33 用例 · 439 断言，0 失败 0 跳过（SKIPPED=0）",
            color=INK, fontsize=13.2, fontweight="bold", va="center")

    layers = [
        ("① 协议层", "15 用例 · 223 断言", "考「C++ 客户端自己是否正确」",
         "进程内假从站回环，不依赖 Python\n功能码 / 异常码 / 半包串包 / 32 位字序", BLUE),
        ("② 适配器契约", "10 用例 · 133 断言", "考「EMS 适配层语义是否正确」",
         "32 点分 3 块读（请求数被钉死）\nFC16 一次原子下发 3 个指令点\n坏数据判不可信而非静默采用", CYAN),
        ("③ 跨语言对账", "8 用例 · 83 断言", "考「两个独立实现能否真正互通」",
         "对端 = 真实 pymodbus 3.15.0 从站\n点表逐点对照 / 指令量测逐拍对账\n对端失联 304 ms 快速失败不挂住", BLUE),
    ]
    for i, (nm, num, exam, note, col) in enumerate(layers):
        x = 0.30 + i * 4.02
        rbox(ax, x, 1.92, 3.80, 1.84, "", fc="white", ec=col, lw=1.4)
        ax.text(x + 0.24, 3.48, nm, ha="left", va="center", color=col,
                fontsize=12.5, fontweight="bold", zorder=5)
        ax.text(x + 0.24, 3.14, num, ha="left", va="center", color=INK,
                fontsize=13.5, fontweight="bold", zorder=5)
        ax.text(x + 0.24, 2.84, exam, ha="left", va="center", color=GRAY,
                fontsize=9.6, zorder=5)
        ax.text(x + 0.24, 2.38, note, ha="left", va="center", color=GRAY,
                fontsize=9.0, zorder=5, linespacing=1.6)

    # 底部两框：活体演示 / 口径复现
    ax.add_patch(FancyBboxPatch((0.30, 0.14), 5.86, 1.56,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=CYAN_BG, edgecolor=CYAN, linewidth=1.3, zorder=3))
    ax.text(0.56, 1.44, "活体演示（现场预演形态，非断言）", ha="left", va="center",
            color=CYAN, fontsize=11.0, fontweight="bold", zorder=4)
    ax.text(0.56, 0.98,
            "起模拟器（负荷 380 kW / 光伏 150 kW）→ 探针连接 27 ms；\n"
            "读回 P_GRID = 232.00（电表读数）、SOC = 0.55，配置区 8 点全读回；\n"
            "从站侧每秒被轮询 10 次 —— 两端隔着 TCP 真实对话，不是函数调用",
            ha="left", va="center", color=INK, fontsize=9.2, zorder=4, linespacing=1.65)

    ax.add_patch(FancyBboxPatch((6.24, 0.14), 5.86, 1.56,
                                boxstyle="round,pad=0,rounding_size=0.10",
                                facecolor=GRAY_BG, edgecolor=BLUE, linewidth=1.3, zorder=3))
    ax.text(6.50, 1.44, "口径复现：两个合法基线在本机都能跑出来", ha="left", va="center",
            color=BLUE, fontsize=11.0, fontweight="bold", zorder=4)
    ax.text(6.50, 0.98,
            "默认口径 9304 的跨语言层只实跑 5 条（7 条因缺环境跳过）；\n"
            "本环境 83 条全部实跑 → 9304 − 5 + 83 = 9382，与装 pymodbus 后官方口径一致；\n"
            "判据：构建日志 SKIPPED=0（9382）还是 SKIPPED=7（9304），报告必须写清口径",
            ha="left", va="center", color=INK, fontsize=9.2, zorder=4, linespacing=1.65)
    save(fig, "sim_tests.png")


def main():
    chart_landing_map()
    chart_modbus_arch()
    chart_crossproc()
    chart_baseline()
    chart_sim_tests()


if __name__ == "__main__":
    main()
