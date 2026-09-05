import pandas as pd
import matplotlib
import matplotlib.pyplot as plt
import os

# 中文字体（Windows 常见中文字体）
for _f in ("Microsoft YaHei", "SimHei", "KaiTi"):
    try:
        matplotlib.rcParams["font.sans-serif"] = [_f]
        matplotlib.rcParams["axes.unicode_minus"] = False
        break
    except Exception:
        continue

SCENARIOS = [
    ("sim_sc1_pv_step.csv", "1-光伏突增 (负载100, PV 50->200)"),
    ("sim_sc2_load_drop.csv", "2-负载突降 (负载200->100, PV250)"),
    ("sim_sc3_pv_osc.csv", "3-光伏波动 (PV 50<->150 三角波, Kff=1.0)"),
    ("sim_sc4_no_reverse.csv", "4-无逆流 (PV<负载)"),
    ("sim_sc5_pv_drop.csv", "5-光伏骤降 (陈旧积分清零)"),
]

if __name__ == "__main__":
    fig, axes = plt.subplots(len(SCENARIOS), 1, figsize=(12, 3.2 * len(SCENARIOS)))
    for ax, (fname, title) in zip(axes, SCENARIOS):
        if not os.path.exists(fname):
            ax.set_title(f"{title}  [缺少文件 {fname}]")
            ax.grid(True)
            continue
        df = pd.read_csv(fname, sep=r"\s+")
        ax.plot(df["Time(s)"], df["P_grid(kW)"], label="P_grid", linewidth=1.5)
        ax.plot(df["Time(s)"], df["P_pv(kW)"], label="P_pv", linewidth=1.2, alpha=0.7)
        ax.plot(df["Time(s)"], df["ChargeReq(kW)"], label="ChargeReq", linewidth=1.5)
        ax.axhline(y=0, color="k", linestyle="--", lw=1)
        ax.set_ylabel("Power(kW)")
        ax.set_title(title)
        ax.legend(loc="best")
        ax.grid(True)
    axes[-1].set_xlabel("Time(s)")
    plt.tight_layout()
    plt.savefig("sim_plot.png", dpi=300, bbox_inches="tight")
    plt.show()

