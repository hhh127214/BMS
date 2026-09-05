"""Generate 96-period (24h @ 15min) benchmark requests and time the server."""
import json
import time
import urllib.request
import urllib.error

URL = "http://127.0.0.1:8000/api/v1/optimize"

# 24-hour profiles (hourly), repeated 4x to make 96 quarters
HOURLY_PRICE = [0.30, 0.25, 0.20, 0.22, 0.35, 0.60, 0.80, 0.90, 0.70, 0.55, 0.45, 0.50,
                0.65, 0.75, 0.85, 0.60, 0.40, 0.30, 0.28, 0.32, 0.45, 0.55, 0.50, 0.38]
HOURLY_LOAD = [40, 38, 36, 35, 34, 36, 42, 55, 70, 85, 92, 95,
               90, 88, 92, 85, 78, 72, 65, 58, 50, 45, 42, 40]
HOURLY_PV = [0, 0, 0, 0, 0, 1, 8, 18, 32, 46, 58, 66,
             70, 68, 60, 46, 30, 14, 3, 0, 0, 0, 0, 0]


def rep(x):
    return [v for v in x for _ in range(4)]


def base(strategy):
    return {
        "strategy": strategy,
        "soc": {"initial": 0.5, "min": 0.1, "max": 0.95, "final": 0.5},
        "battery": {"capacity_kwh": 100.0, "max_charge_kw": 60.0, "max_discharge_kw": 60.0,
                    "charge_efficiency": 0.95, "discharge_efficiency": 0.95},
        "price": rep(HOURLY_PRICE),
        "time_step_hours": 0.25,
    }


def post(req):
    data = json.dumps(req).encode("utf-8")
    r = urllib.request.Request(URL, data=data, headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        resp = urllib.request.urlopen(r)
        body = resp.read()
    except urllib.error.HTTPError as e:
        body = e.read()
    dt = time.perf_counter() - t0
    j = json.loads(body)
    return j, dt


def main():
    cases = []
    r = base("arbitrage")
    cases.append(("arbitrage_96", r))

    r = base("forecast")
    r["load_kw"] = rep(HOURLY_LOAD)
    r["pv_kw"] = rep(HOURLY_PV)
    cases.append(("forecast_96", r))

    r = base("demand_response")
    r["load_kw"] = rep(HOURLY_LOAD)
    r["dr_signal"] = [1 if 9 <= (i // 4) <= 15 else 0 for i in range(96)]
    r["demand_price_per_kw"] = 30.0
    r["demand_limit_kw"] = 88.0
    cases.append(("demand_response_96", r))

    for name, req in cases:
        j, dt = post(req)
        n = len(req["price"])
        if j.get("status") == "ok":
            print(f"{name}: n={n} time={dt*1000:.0f} ms exact={j['exact']} "
                  f"nodes={j['nodes_used']} "
                  f"profit={j.get('total_profit_yuan')} cost={j.get('total_cost_yuan')}")
        else:
            print(f"{name}: n={n} time={dt*1000:.0f} ms ERROR {j.get('message')}")


if __name__ == "__main__":
    main()
