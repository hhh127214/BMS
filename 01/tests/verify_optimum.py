"""Brute-force OPTIMALITY check for the C MILP solver.

For small random cases (n=4) it enumerates all 2^n charging-binary
patterns, solves the resulting LP per pattern with scipy.optimize.linprog,
and compares the best objective against the C MILP served over HTTP.

Usage:  python verify_optimum.py   (requires scipy; server must run)
"""

import itertools
import json
import random
import urllib.request

import numpy as np
from scipy.optimize import linprog

URL = "http://127.0.0.1:8000/api/v1/optimize"


def solve_pattern(strategy, n, req, u):
    """Solve the LP for one fixed binary pattern u (list of 0/1)."""
    dt = req.get("time_step_hours", 1.0)
    cap = req["battery"]["capacity_kwh"]
    p_chg = req["battery"]["max_charge_kw"]
    p_dis = req["battery"]["max_discharge_kw"]
    etac = req["battery"]["charge_efficiency"]
    etad = req["battery"]["discharge_efficiency"]
    si = req["soc"]["initial"]
    smin = req["soc"]["min"]
    smax = req["soc"]["max"]
    sf = req["soc"].get("final", si)
    price = req["price"]

    has_grid = strategy != "arbitrage"
    nv = 3 * n + (n if has_grid else 0) + (1 if strategy == "demand_response" else 0)

    def chg(t): return 3 * t
    def dis(t): return 3 * t + 1
    def soc(t): return 3 * t + 2
    def grid(t): return 3 * n + t
    def m(): return 3 * n + n

    c = [0.0] * nv
    # linprog minimizes; we must minimize the NEGATIVE of C's maximization
    # objective so that -res.fun equals C's optimum.
    if strategy == "arbitrage":
        for t in range(n):
            c[chg(t)] = price[t] * dt        # was -price (maximize profit)
            c[dis(t)] = -price[t] * dt
    else:
        for t in range(n):
            c[grid(t)] = price[t] * dt       # minimize energy cost
        if strategy == "demand_response":
            c[m()] = req.get("demand_price_per_kw", 10.0)

    A_ub, b_ub = [], []
    for t in range(n):
        row = [0.0] * nv; row[chg(t)] = 1.0; b_ub.append(p_chg * u[t]); A_ub.append(row)
        row = [0.0] * nv; row[dis(t)] = 1.0; b_ub.append(p_dis * (1 - u[t])); A_ub.append(row)
        row = [0.0] * nv; row[soc(t)] = 1.0; b_ub.append(smax); A_ub.append(row)
        row = [0.0] * nv; row[soc(t)] = -1.0; b_ub.append(-smin); A_ub.append(row)

    A_eq, b_eq = [], []
    for t in range(n):
        row = [0.0] * nv
        row[soc(t)] = 1.0
        row[chg(t)] = -etac * dt / cap
        row[dis(t)] = dt / (etad * cap)
        if t > 0:
            row[soc(t - 1)] = -1.0
        b_eq.append(si if t == 0 else 0.0)
        A_eq.append(row)
    row = [0.0] * nv
    row[soc(n - 1)] = 1.0
    b_eq.append(sf)
    A_eq.append(row)

    if has_grid:
        load = req.get("load_kw", req.get("load"))
        pv = req.get("pv_kw", req.get("pv", [0.0] * n))
        for t in range(n):
            row = [0.0] * nv
            row[grid(t)] = 1.0
            row[chg(t)] = -1.0
            row[dis(t)] = 1.0
            b_eq.append(load[t] - pv[t])
            A_eq.append(row)
        if strategy == "demand_response":
            dr_signal = req.get("dr_signal")
            for t in range(n):
                if dr_signal and dr_signal[t] == 0:
                    continue
                row = [0.0] * nv
                row[grid(t)] = 1.0
                row[m()] = -1.0
                b_ub.append(0.0)
                A_ub.append(row)
            if req.get("demand_limit_kw", 0.0) > 0.0:
                for t in range(n):
                    row = [0.0] * nv
                    row[grid(t)] = 1.0
                    b_ub.append(req["demand_limit_kw"])
                    A_ub.append(row)

    res = linprog(c, A_ub=np.array(A_ub), b_ub=np.array(b_ub),
                  A_eq=np.array(A_eq), b_eq=np.array(b_eq),
                  bounds=[(0, None)] * nv, method="highs")
    if res.success:
        return -res.fun
    return None
def post(req):
    data = json.dumps(req).encode("utf-8")
    request = urllib.request.Request(URL, data=data, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(request)
        return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return json.loads(e.read().decode("utf-8"))


def random_req(strategy, seed):
    n = 4
    rnd = random.Random(seed)
    req = {
        "strategy": strategy,
        "soc": {"initial": 0.5, "min": 0.1, "max": 0.9, "final": 0.5},
        "battery": {
            "capacity_kwh": round(rnd.uniform(5, 40), 1),
            "max_charge_kw": round(rnd.uniform(2, 10), 1),
            "max_discharge_kw": round(rnd.uniform(2, 10), 1),
            "charge_efficiency": 0.95,
            "discharge_efficiency": 0.95,
        },
        "price": [round(rnd.uniform(0.15, 1.2), 3) for _ in range(n)],
        "time_step_hours": 1.0,
    }
    if strategy != "arbitrage":
        req["load_kw"] = [round(rnd.uniform(5, 20), 1) for _ in range(n)]
        req["pv_kw"] = [round(rnd.uniform(0, 6), 1) for _ in range(n)]
    if strategy == "demand_response":
        req["demand_price_per_kw"] = 20.0
        req["dr_signal"] = [1, 0, 1, 0]
        # 不设硬性 demand_limit_kw：随机小电池+低限值会导致不可行，
        # 为保证 12/12 算例全部可行、散点图能画出 12 个点，这里不施加硬限
    return req


def main():
    total = 0
    fails = 0
    seed = 1000
    for strategy in ["arbitrage", "forecast", "demand_response"]:
        for case in range(4):
            req = random_req(strategy, seed)
            seed += 1
            n = len(req["price"])
            best = None
            for u in itertools.product([0, 1], repeat=n):
                v = solve_pattern(strategy, n, req, list(u))
                if v is not None and (best is None or v > best):
                    best = v
            resp = post(req)
            total += 1
            if best is None:
                if resp.get("status") == "error":
                    print(f"[skip] {strategy} case{case}: infeasible both sides")
                else:
                    fails += 1
                    print(f"[FAIL] {strategy} case{case}: brute says infeasible, C says {resp.get('status')}")
                continue
            got = resp.get("total_profit_yuan") if strategy == "arbitrage" else -resp.get("total_cost_yuan")
            if got is None:
                fails += 1
                print(f"[FAIL] {strategy} case{case}: C error: {resp}")
                continue
            if abs(got - best) > 1e-3:
                fails += 1
                print(f"[FAIL] {strategy} case{case}: brute={best:.6f} C={got:.6f}")
                with open("debug_req.json", "w", encoding="utf-8") as f:
                    json.dump(req, f, indent=2, ensure_ascii=False)
                print("  -> request dumped to debug_req.json")
            else:
                print(f"[ok] {strategy} case{case}: brute={best:.6f} C={got:.6f} nodes={resp.get('nodes_used')}")
    print(f"\n{total - fails}/{total} cases matched optimality")
    if fails:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

