#!/usr/bin/env python3
"""Drive yaml/test_nowifi_sweep.yaml: for each config call set_scan, then nowifi_test (WiFi off for N s).
Usage: nowifi_sweep.py <host> <seconds-per-config> [config-index ...]   (log is captured over USB separately)"""
import asyncio, sys, time
from aioesphomeapi import APIClient, APIConnectionError

CONFIGS = [
    # label, interval_1m, window_1m, active_1m, interval_coded, window_coded, phy_mask(1=1M,2=coded,3=both)
    ("A both 1M400/80act coded400/300", 400, 80, True, 400, 300, 3),
    ("B coded-only 400/400", 400, 30, True, 400, 400, 2),
    ("C coded-only 30/30", 400, 30, True, 30, 30, 2),
    ("D coded-only 100/100", 400, 30, True, 100, 100, 2),
    ("E both 1M400/30act coded400/370", 400, 30, True, 400, 370, 3),
    ("F coded-only 1000/1000", 400, 30, True, 1000, 1000, 2),
    # round 2: around the best result (coded-only 400/400 = 74 % without WiFi)
    ("G coded-only 200/200", 400, 30, True, 200, 200, 2),
    ("H coded-only 300/300", 400, 30, True, 300, 300, 2),
    ("I coded-only 400/400 repeat", 400, 30, True, 400, 400, 2),
    ("J coded-only 500/500", 400, 30, True, 500, 500, 2),
    ("K coded-only 600/600", 400, 30, True, 600, 600, 2),
    ("L both 1M400/20act coded400/380", 400, 20, True, 400, 380, 3),
]

async def connect(host, tries=150):
    for _ in range(tries):
        c = APIClient(host, 6053, None)
        try:
            await c.connect(login=True)
            return c
        except (APIConnectionError, OSError):
            await asyncio.sleep(5)
    raise SystemExit("device not reachable")

async def main():
    host = sys.argv[1]; secs = int(sys.argv[2])
    idx = [int(x) for x in sys.argv[3:]] or list(range(len(CONFIGS)))
    for i in idx:
        label, i1, w1, a1, ic, wc, mask = CONFIGS[i]
        c = await connect(host)
        _, services = await c.list_entities_services()
        svc = {s.name: s for s in services}
        await c.execute_service(svc["set_scan"], {"interval_1m": i1, "window_1m": w1, "active_1m": a1,
                                                  "interval_coded": ic, "window_coded": wc, "phy_mask": mask})
        await asyncio.sleep(5)
        await c.execute_service(svc["nowifi_test"], {"label": label, "seconds": secs})
        print(f"{time.strftime('%H:%M:%S')} started: {label}", flush=True)
        try:
            await c.disconnect()
        except Exception:
            pass
        await asyncio.sleep(secs + 45)
    print("sweep done", flush=True)

asyncio.run(main())
