#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
10/ 典型日曲线生成器 —— 96 点（15 min）× 24 h
输出：data/typical_day_96.csv

列：idx,time_h,price_cny_kwh,p_load_kw,p_pv_kw

口径与 07/08/09 的 ForecastSeries 完全一致（阶梯保持采样），
便于 10/ 离线仿真与闭环测试用同一条曲线做对照。
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "data", "typical_day_96.csv")

# 分时电价（工商业两部制，单位：元/kWh）
#   谷 0.30 | 平 0.60 | 峰 1.00 | 尖 1.20
def price_at(h):
    if h < 7.0:   return 0.30
    if h < 9.0:   return 0.60
    if h < 12.0:  return 1.00
    if h < 14.0:  return 0.60
    if h < 17.0:  return 1.00
    if h < 21.0:  return 1.20
    if h < 23.0:  return 0.60
    return 0.30

# 双峰负荷：基础 250 kW，上午峰 ~400，晚峰 ~450
def load_at(h):
    return (250.0
            + 150.0 * math.exp(-((h - 10.0) ** 2) / 8.0)
            + 200.0 * math.exp(-((h - 19.0) ** 2) / 6.0))

# 光伏：6:00-18:00 正弦，峰值 300 kW
def pv_at(h):
    if 6.0 <= h <= 18.0:
        return 300.0 * math.sin(math.pi * (h - 6.0) / 12.0)
    return 0.0


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    rows = []
    for i in range(96):
        h = i * 0.25
        rows.append((i, h, price_at(h), load_at(h), pv_at(h)))

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 典型日曲线 96 点（15 min）：idx,time_h,price_cny_kwh,p_load_kw,p_pv_kw\n")
        f.write("idx,time_h,price_cny_kwh,p_load_kw,p_pv_kw\n")
        for i, h, pr, ld, pv in rows:
            f.write("%d,%.2f,%.4f,%.2f,%.2f\n" % (i, h, pr, ld, pv))

    ld_max = max(r[3] for r in rows)
    pv_max = max(r[4] for r in rows)
    e_load = sum(r[3] for r in rows) * 0.25
    e_pv = sum(r[4] for r in rows) * 0.25
    print("written:", os.path.normpath(OUT))
    print("  load_max=%.1f kW  pv_max=%.1f kW" % (ld_max, pv_max))
    print("  E_load=%.1f kWh  E_pv=%.1f kWh" % (e_load, e_pv))


if __name__ == "__main__":
    main()
