"""Validate that the returned charge/discharge plan is FEASIBLE.

Usage (with the server running):
    python verify_plan.py [request_file] [url]

Checks (for all strategies): plan length, SOC within [min, max], power
within limits, no simultaneous charge/discharge, consistent SOC trace,
final SOC == target. For forecast/demand_response additionally checks the
grid balance, grid >= 0, peak and demand-limit constraints.
"""

import json
import sys
import urllib.request

DEFAULT_URL = "http://127.0.0.1:8000/api/v1/optimize"
DEFAULT_REQ = "sample_request.json"

TOL = 1e-4


def main():
    url = DEFAULT_URL
    req_file = DEFAULT_REQ
    if len(sys.argv) > 1:
        req_file = sys.argv[1]
    if len(sys.argv) > 2:
        url = sys.argv[2]

    with open(req_file, "r", encoding="utf-8") as f:
        req = json.load(f)

    data = json.dumps(req).encode("utf-8")
    r = urllib.request.urlopen(
        urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    )
    resp = json.loads(r.read().decode("utf-8"))

    errors = []
    n = len(req["price"])
    strategy = req.get("strategy", "arbitrage")
    soc0 = req["soc"]["initial"]
    soc_min = req["soc"]["min"]
    soc_max = req["soc"]["max"]
    soc_final = req["soc"].get("final", soc0)
    cap = req["battery"]["capacity_kwh"]
    p_chg = req["battery"]["max_charge_kw"]
    p_dis = req["battery"]["max_discharge_kw"]
    eta_chg = req["battery"]["charge_efficiency"]
    eta_dis = req["battery"]["discharge_efficiency"]
    dt = req.get("time_step_hours", 1.0)

    plan = resp.get("plan", [])
    if len(plan) != n:
        errors.append(f"plan length {len(plan)} != price length {n}")

    load = req.get("load_kw", req.get("load"))
    pv = req.get("pv_kw", req.get("pv", [0.0] * n))
    dr_signal = req.get("dr_signal")
    demand_limit = req.get("demand_limit_kw", 0.0)

    grid = [None] * n
    soc = soc0
    peak = 0.0
    for item in plan:
        t = item["t"]
        c = item.get("charge_kw", 0.0)
        d = item.get("discharge_kw", 0.0)
        s = item.get("soc", 0.0)
        if c < -1e-6 or d < -1e-6:
            errors.append(f"negative power at t={t}")
        if c > 1e-6 and d > 1e-6:
            errors.append(f"simultaneous charge/discharge at t={t}")
        if c > p_chg + TOL:
            errors.append(f"charge power {c} > {p_chg} at t={t}")
        if d > p_dis + TOL:
            errors.append(f"discharge power {d} > {p_dis} at t={t}")
        soc += (c * dt * eta_chg - d * dt / eta_dis) / cap
        if s < soc_min - TOL or s > soc_max + TOL:
            errors.append(f"soc {s} out of bounds at t={t}")
        if abs(s - soc) > TOL:
            errors.append(f"soc field mismatch at t={t} (calc {soc})")

    if abs(soc - soc_final) > TOL:
        errors.append(f"final soc {soc} != target {soc_final}")

    if strategy != "arbitrage":
        if load is None:
            errors.append("no load_kw in request")
        for item in plan:
            t = item["t"]
            g = item.get("grid_kw", None)
            if g is None:
                errors.append(f"missing grid_kw at t={t}")
                continue
            if g < -TOL:
                errors.append(f"grid {g} < 0 at t={t}")
            if g > peak:
                peak = g
            grid[t] = g
            expected = load[t] - (pv[t] if pv else 0.0) + item["charge_kw"] - item["discharge_kw"]
            if abs(g - expected) > TOL:
                errors.append(f"grid balance mismatch at t={t}: {g} vs {expected}")
        if demand_limit > 0.0:
            for t, g in enumerate(grid):
                if g is not None and g > demand_limit + TOL:
                    errors.append(f"grid {g} exceeds demand_limit {demand_limit} at t={t}")
        if "peak_kw" in resp and abs(resp["peak_kw"] - peak) > TOL:
            errors.append(f"reported peak_kw {resp['peak_kw']} != actual {peak}")
        if dr_signal:
            dr_peak = max((grid[t] for t in range(n) if dr_signal[t] == 1), default=0.0)
            if "dr_peak_kw" in resp and abs(resp["dr_peak_kw"] - dr_peak) > TOL:
                errors.append(f"reported dr_peak_kw {resp['dr_peak_kw']} != actual {dr_peak}")

    if errors:
        print(f"VIOLATIONS ({strategy}):")
        for e in errors:
            print(" -", e)
        sys.exit(1)

    extra = f", profit={resp.get('total_profit_yuan')}" if strategy == "arbitrage" \
        else f", cost={resp.get('total_cost_yuan')}, peak={resp.get('peak_kw')}"
    print(f"OK [{strategy}]: {n} hours, all feasible{extra}")


if __name__ == "__main__":
    main()
