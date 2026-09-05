"""Minimal client for the pure-C battery optimizer service.

Usage (with the server running on port 8000):
    python test_client.py [request_file] [url]
"""

import json
import sys
import urllib.request

DEFAULT_URL = "http://127.0.0.1:8000/api/v1/optimize"
DEFAULT_REQ = "sample_request.json"

SAMPLE_REQUEST = {
    "strategy": "arbitrage",
    "soc": {"initial": 0.5, "min": 0.2, "max": 0.9, "final": 0.5},
    "battery": {
        "capacity_kwh": 100.0,
        "max_charge_kw": 50.0,
        "max_discharge_kw": 50.0,
        "charge_efficiency": 0.95,
        "discharge_efficiency": 0.95,
    },
    "price": [
        0.30, 0.25, 0.20, 0.22, 0.35, 0.60,
        0.80, 0.90, 0.70, 0.55, 0.45, 0.50,
        0.65, 0.75, 0.85, 0.60, 0.40, 0.30,
        0.28, 0.32, 0.45, 0.55, 0.50, 0.38,
    ],
    "time_step_hours": 1.0,
}


def main():
    url = DEFAULT_URL
    req_file = None
    if len(sys.argv) > 1:
        req_file = sys.argv[1]
    if len(sys.argv) > 2:
        url = sys.argv[2]
    if req_file:
        with open(req_file, "r", encoding="utf-8") as f:
            payload = json.load(f)
    else:
        payload = SAMPLE_REQUEST
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req) as resp:
        body = resp.read().decode("utf-8")
    print(json.dumps(json.loads(body), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
