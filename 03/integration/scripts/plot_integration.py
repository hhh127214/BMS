import pandas as pd
import matplotlib.pyplot as plt
import os
# 读取CSV
script_dir = os.path.dirname(os.path.abspath(__file__))
csv_path = os.path.join(script_dir, 'integration_sim.csv')
df = pd.read_csv(csv_path)

# ========== 绘图专用转换，不改动原始数据 ==========
def transform_smooth_for_plot(sm_raw):
    if sm_raw < 0:
        # 充电请求翻转成正数，和Anti‑Reverse坐标轴方向对齐
        return -sm_raw
    else:
        # 放电请求翻转成负数
        return -sm_raw

def transform_demand_for_plot(dem_raw):
    # Demand原始：>0代表放电请求；绘图取负，视觉方向对齐
    return -dem_raw

df["Sm_plot"] = df["Sm_kW"].apply(transform_smooth_for_plot)
df["Dem_plot"] = df["Dem_kW"].apply(transform_demand_for_plot)
# =============================================================

fig, axes = plt.subplots(4, 1, figsize=(12, 10), sharex=True)
# ---------- 第一幅：功率总览 ----------
axes[0].plot(df['Time_s'], df['P_load_kW'], label='Load', linestyle='--', color='black')
axes[0].plot(df['Time_s'], df['P_pv_kW'], label='PV', color='orange')
axes[0].plot(df['Time_s'], df['P_grid_kW'], label='Grid', color='blue')
axes[0].axhline(250, color='red', linestyle=':', linewidth=1, label='D_target = 250 kW')
axes[0].axhline(0, color='green', linestyle=':', linewidth=1, label='P_grid_min = 0 kW')
axes[0].set_ylabel('Power (kW)')
axes[0].legend()
axes[0].grid(True)
axes[0].set_title('Integrated Simulation (Step Response)')

# ---------- 第二幅：储能功率 ----------
axes[1].plot(df['Time_s'], df['P_bat_kW'], label='Battery Power', color='black')
axes[1].axhline(0, color='gray', linewidth=0.5)
axes[1].set_ylabel('Power (kW)')
axes[1].legend()
axes[1].grid(True)

# ---------- 第三幅：控制器输出，Dem使用转换后的Dem_plot ----------
axes[2].plot(df['Time_s'], df['Rev_kW'], label='Anti‑Reverse', color='red')
axes[2].plot(df['Time_s'], df['Dem_plot'], label='Demand(visual transformed)', color='blue')
axes[2].plot(df['Time_s'], df["Sm_plot"], label='Smoothing(visual transformed)', color='green')
axes[2].axhline(0, color='gray', linewidth=0.5)
axes[2].set_ylabel('Controller Output (kW)')
axes[2].legend()
axes[2].grid(True)

# ---------- 第四幅：SOC ----------
axes[3].plot(df['Time_s'], df['SOC_pct'], label='SOC', color='purple')
axes[3].axhline(80, color='red', linestyle=':', linewidth=1, label='SOC_high = 80%')
axes[3].axhline(20, color='blue', linestyle=':', linewidth=1, label='SOC_low = 20%')
axes[3].set_xlabel('Time (s)')
axes[3].set_ylabel('SOC (%)')
axes[3].legend()
axes[3].grid(True)

plt.tight_layout()
plt.savefig(os.path.join(script_dir, 'integration_test.png'), dpi=150, bbox_inches='tight')
plt.show()
