#!/usr/bin/env python3
"""Sweep extended-scan parameters at runtime (api actions set_scan/set_coex) and measure the Coded-PHY report
rate from the "BLE Reports Coded" / "BLE Reports 1M" sensors (published once per stats_interval = 60 s).
Only the state subscription is used, so WiFi traffic stays minimal during the measurement.
Usage: sweep.py <host> <minutes-per-config> [config-index ...]"""
import asyncio, sys, time
from aioesphomeapi import APIClient

CONFIGS = [
    # name, interval_1m, window_1m, active_1m, interval_coded, window_coded, prefer_bt, phy_mask(1=1M,2=coded,3=both)
    ("coded only 100/95", 400, 30, True, 100, 95, False, 2),
    ("coded only 400/400", 400, 30, True, 400, 400, False, 2),
    ("coded only 100/100", 400, 30, True, 100, 100, False, 2),
    ("coded only 30/30", 400, 30, True, 30, 30, False, 2),
    ("coded only 100/95, coex BT", 400, 30, True, 100, 95, True, 2),
    ("coded only 1000/1000", 400, 30, True, 1000, 1000, False, 2),
    ("both: 1M 400/30, coded 100/95", 400, 30, True, 100, 95, False, 3),
    ("coded only 50/50", 400, 30, True, 50, 50, False, 2),
]

async def main():
    host = sys.argv[1]; minutes = float(sys.argv[2])
    idx = [int(x) for x in sys.argv[3:]] or list(range(len(CONFIGS)))
    c = APIClient(host, 6053, None)
    await c.connect(login=True)
    ents, services = await c.list_entities_services()
    keys = {e.name: e.key for e in ents}
    svc = {s.name: s for s in services}
    samples = {}
    def cb(s):
        samples.setdefault(s.key, []).append((time.time(), getattr(s, "state", None)))
    c.subscribe_states(cb)
    results = []
    for i in idx:
        name, i1, w1, a1, ic, wc, bt, mask = CONFIGS[i]
        await c.execute_service(svc["set_coex"], {"prefer_bt": bt})
        await c.execute_service(svc["set_scan"], {"interval_1m": i1, "window_1m": w1, "active_1m": a1,
                                            "interval_coded": ic, "window_coded": wc, "phy_mask": mask})
        t_start = time.time() + 65  # ignore the first (partial) stats window
        await asyncio.sleep(65 + minutes * 60)
        def avg(key):
            v = [s for t, s in samples.get(keys[key], []) if t >= t_start]
            return (sum(v) / len(v), len(v)) if v else (float("nan"), 0)
        coded, n = avg("BLE Reports Coded"); m1, _ = avg("BLE Reports 1M")
        heap, _ = avg("Free Heap")
        line = f"{i}: {name:42s} coded={coded:5.1f}/min ({n} samples) 1M={m1:6.1f}/min heap={heap:.0f}"
        print(line, flush=True); results.append(line)
    print("==== summary ====")
    for r in results: print(r)
    await c.disconnect()

asyncio.run(main())
