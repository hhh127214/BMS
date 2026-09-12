#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 08/data/day_plan_sample.json —— 模拟 01/ MILP 优化层产出的日间运行计划。

用途：
  周期 8 的协同演示优先加载本文件（走"01/ 优化层 → 实时层纠偏 → 安全层兜底"的真实链路），
  加载失败时才降级到内置贪心兜底优化器（build_plan_greedy）。

输出格式与 01/docs/api.md 的 MILP HTTP 响应一致：
  { "status","strategy","horizon","time_step_hours","final_soc","total_profit_yuan",
    "plan":[ {"t","charge_kw","discharge_kw","soc","grid_kw"}, ... ] }

约定（全系统一致）：
  P_bat 放电为正、充电为负；P_grid 取电为正、倒送为负；
  P_grid = P_load - P_pv - P_bat
"""
import json
import math
import os

STEP_H = 0.25
N = 96                      # 96 × 15min = 24h
CAP_KWH = 1000.0
SOC_MIN, SOC_MAX = 0.10, 0.90
SOC_INIT = 0.50
P_CHG_MAX = P_DIS_MAX = 250.0
ETA = 0.95
GRID_MAX = 350.0            # 契约需量（并网硬边界）
GRID_MIN = 0.0              # 不允许倒送


def price_of(h):
    if h < 7.0:   return 0.30
    if h < 9.0:   return 0.60
    if h < 12.0:  return 1.00
    if h < 14.0:  return 0.60
    if h < 17.0:  return 1.00
    if h < 21.0:  return 1.20
    if h < 23.0:  return 0.60
    return 0.30


def load_of(h):
    return (180.0
            + 120.0 * math.exp(-((h - 10.0) ** 2) / 8.0)
            + 160.0 * math.exp(-((h - 19.0) ** 2) / 6.0)
            + 40.0 * math.sin(2.0 * math.pi * h / 24.0))


def pv_of(h):
    return 300.0 * math.sin(math.pi * (h - 6.0) / 12.0) if 6.0 <= h <= 18.0 else 0.0


def main():
    load = [load_of(i * STEP_H) for i in range(N)]
    pv = [pv_of(i * STEP_H) for i in range(N)]
    price = [price_of(i * STEP_H) for i in range(N)]
    net = [load[i] - pv[i] for i in range(N)]

    # ---- 动作分配 ----
    action = [0] * N                      # -1 充 / +1 放 / 0 待机
    for i in range(N):
        if net[i] < 0.0:
            action[i] = -1                # 光伏余电必须吸收
    for i in range(N):
        if action[i] == 0 and price[i] >= 1.00:
            action[i] = +1                # 峰段放电
    for i in range(N):
        if action[i] == 0 and price[i] <= 0.30:
            action[i] = -1                # 谷段充电

    # ---- 为后续光伏余电预留 SOC 裕度 ----
    surplus_ahead = [0.0] * (N + 1)
    for i in range(N - 1, -1, -1):
        e = min(P_CHG_MAX, -net[i]) * STEP_H if net[i] < 0.0 else 0.0
        surplus_ahead[i] = surplus_ahead[i + 1] + e
    usable = CAP_KWH * ETA

    def soc_cap(i):
        reserve = min(SOC_MAX - SOC_MIN, surplus_ahead[i + 1] / usable)
        return max(SOC_MIN, SOC_MAX - reserve)

    # ---- 正演：SOC 递推 + 并网/功率限幅 ----
    soc = SOC_INIT
    plan = []
    for i in range(N):
        p = 0.0
        if action[i] == -1:
            cap = soc_cap(i)
            room_kwh = max(0.0, (cap - soc) * CAP_KWH) / ETA
            want = min(P_CHG_MAX, -net[i]) if net[i] < 0.0 else P_CHG_MAX
            p = -min(want, room_kwh / STEP_H)
        elif action[i] == +1:
            avail = max(0.0, (soc - SOC_MIN) * CAP_KWH) * ETA
            p = min(P_DIS_MAX, avail / STEP_H)
            p = min(p, max(0.0, net[i]))          # 不允许倒送
        # 并网容量约束（L1 硬边界）：base - p <= GRID_MAX
        if net[i] - p > GRID_MAX:
            p = net[i] - GRID_MAX
        p = max(-P_CHG_MAX, min(P_DIS_MAX, p))

        soc_next = soc
        if p > 0.0:
            soc_next = soc - p * STEP_H / CAP_KWH / ETA
        elif p < 0.0:
            soc_next = soc + (-p) * STEP_H * ETA / CAP_KWH
        soc = max(SOC_MIN, min(SOC_MAX, soc_next))

        chg = max(0.0, -p)
        dis = max(0.0, p)
        plan.append({
            "t": i,
            "charge_kw": round(chg, 3),
            "discharge_kw": round(dis, 3),
            "soc": round(soc, 5),
            "grid_kw": round(net[i] - p, 3),
        })

    total_profit = sum(
        (plan[i]["discharge_kw"] - plan[i]["charge_kw"]) * STEP_H * price[i]
        for i in range(N)
    )

    out = {
        "status": "ok",
        "strategy": "arbitrage_peak_valley",
        "exact": 1,
        "horizon": N,
        "time_step_hours": STEP_H,
        "final_soc": round(soc, 5),
        "total_profit_yuan": round(total_profit, 2),
        "note": "模拟 01/ MILP 产出；契约需量 350kW、不允许倒送、SOC[0.10,0.90]",
        "plan": plan,
    }

    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "data", "day_plan_sample.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)

    chg = sum(p["charge_kw"] for p in plan) * STEP_H
    dis = sum(p["discharge_kw"] for p in plan) * STEP_H
    print("wrote %s" % os.path.normpath(path))
    print("  horizon=%d step=%.2fh  计划充电 %.1f kWh / 放电 %.1f kWh"
          % (N, STEP_H, chg, dis))
    print("  末态 SOC %.3f  预估收益 %.2f 元" % (soc, total_profit))


if __name__ == "__main__":
    main()
