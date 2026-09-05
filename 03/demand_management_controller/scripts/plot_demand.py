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

script_dir = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = [
    ("demand_sim.csv", "1-综合工况 (启动/需量动作/通信中断/禁止放电/P_dis_max=0)"),
    ("demand_sim_over.csv", "2-持续高负荷-已越限 (负载450 > D_target+P_dis_max)"),
    ("demand_sim_perf.csv", "3-纯需量动作 (PR-02验证: 滚动平均±1%)"),
    ("demand_sim_long.csv", "4-长时需量 (长期稳定: A精确收敛D_target)"),
]

for fname, title in SCENARIOS:
    csv_path = os.path.join(script_dir, fname)
    if not os.path.exists(csv_path):
        print(f"[跳过] 缺少 {fname}")
        continue
    df = pd.read_csv(csv_path)

    fig, axes = plt.subplots(3, 1, figsize=(12, 10))
    # 第一幅：负载、并网功率、目标需量
    ax = axes[0]
    ax.plot(df['Time_s'], df['Load_kW'], label='Load', color='gray', linestyle='--', linewidth=1)
    ax.plot(df['Time_s'], df['P_grid_kW'], label='P_grid', color='blue', linewidth=1.5)
    ax.axhline(y=250, color='red', linestyle=':', linewidth=2, label='D_target = 250 kW')
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Power (kW)')
    ax.legend()
    ax.grid(True)
    ax.set_title(title)

    # 第二幅：储能放电功率与控制器输出（含告警）
    ax = axes[1]
    ax.plot(df['Time_s'], df['P_bat_kW'], label='P_bat (Discharge)', color='green', linewidth=1.5)
    ax.plot(df['Time_s'], df['P_req_kW'], label='P_req (Controller Output)', color='orange', linestyle='--')
    if 'alarm' in df.columns:
        ax.plot(df['Time_s'], df['alarm'] * 200, label='alarm (x200)', color='red', linestyle=':', linewidth=1)
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Power (kW)')
    ax.legend()
    ax.grid(True)

    # 第三幅：滑动窗口平均功率
    ax = axes[2]
    ax.plot(df['Time_s'], df['A_window_kW'], label='Window Average Power', color='purple', linewidth=1.5)
    ax.axhline(y=250, color='red', linestyle=':', linewidth=2, label='D_target')
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Power (kW)')
    ax.legend()
    ax.grid(True)

    plt.tight_layout()
    out = os.path.join(script_dir, os.path.splitext(fname)[0] + "_response.png")
    plt.savefig(out, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print("saved", out)
