import pandas as pd
import matplotlib.pyplot as plt
import os
import glob

def plot_and_save_scenario(csv_path):
    """读取单个场景CSV并保存PNG"""
    scenario = os.path.basename(csv_path).replace('.csv', '').replace('smoothing_', '')
    print(f"正在处理场景: {scenario}")

    df = pd.read_csv(csv_path)

    # 创建图形
    plt.figure(figsize=(12, 8))

    # 第一幅：光伏、平滑、并网功率
    plt.subplot(3, 1, 1)
    plt.plot(df['Time_s'], df['P_pv_kW'], label='P_pv (Raw)', color='gray', alpha=0.6, linewidth=1)
    plt.plot(df['Time_s'], df['P_smooth_kW'], label='P_smooth (Filtered)', color='blue', linewidth=1.5)
    plt.plot(df['Time_s'], df['P_grid_kW'], label='P_grid (Actual)', color='red', linestyle='--', linewidth=1)
    plt.xlabel('Time (s)')
    plt.ylabel('Power (kW)')
    plt.legend()
    plt.grid(True)
    plt.title(f'Scenario: {scenario}')

    # 第二幅：储能补偿功率
    plt.subplot(3, 1, 2)
    plt.plot(df['Time_s'], df['P_comp_kW'], label='P_comp', color='green', linewidth=1.5)
    plt.axhline(y=0, color='k', linestyle=':', linewidth=1)
    plt.xlabel('Time (s)')
    plt.ylabel('Power (kW)')
    plt.legend()
    plt.grid(True)

    # 第三幅：SOC与状态标志
    plt.subplot(3, 1, 3)
    plt.plot(df['Time_s'], df['SOC_pct'], label='SOC', color='purple', linewidth=1.5)
    plt.axhline(y=80, color='red', linestyle=':', linewidth=1, label='SOC_high=80%')
    plt.axhline(y=20, color='blue', linestyle=':', linewidth=1, label='SOC_low=20%')
    # 通信中断区间
    plt.fill_between(df['Time_s'], 0, 100, where=(df['data_valid'] == 0),
                     color='orange', alpha=0.3, label='Data Invalid')
    # 平抑关闭区间
    plt.fill_between(df['Time_s'], 0, 100, where=(df['smoothing_enabled'] == 0),
                     color='gray', alpha=0.3, label='Smoothing Disabled')
    plt.xlabel('Time (s)')
    plt.ylabel('SOC (%) / Status')
    plt.legend()
    plt.grid(True)

    # 保存图片
    png_filename = f"{scenario}.png"
    plt.tight_layout()
    plt.savefig(png_filename, dpi=150, bbox_inches='tight')
    plt.close()  # 释放内存
    print(f"已保存: {png_filename}")

def main():
    # 获取当前脚本所在目录
    script_dir = os.path.dirname(os.path.abspath(__file__))
    # 查找所有场景CSV
    csv_files = sorted(glob.glob(os.path.join(script_dir, 'smoothing_S*.csv')))

    if not csv_files:
        print("未找到场景CSV文件！请先运行仿真程序。")
        return

    print(f"找到 {len(csv_files)} 个场景文件。")
    for csv_path in csv_files:
        plot_and_save_scenario(csv_path)

    print("所有场景图片已保存完毕。")

if __name__ == "__main__":
    main()