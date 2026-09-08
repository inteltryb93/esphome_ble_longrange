#!/usr/bin/env python3
"""Subscribe to the bluetooth_proxy raw-advertisement stream (what Home Assistant's Bluetooth integration gets)
and report which addresses arrive, with the BTHome service data for the given MAC.
Usage: proxy_check.py <host> <seconds> [MAC]

WARNING: an ESPHome bluetooth_proxy has ONE advertisement subscriber slot ("newest subscriber wins",
bluetooth_proxy.cpp subscribe_api_connection). Running this script while Home Assistant is connected takes the
slot away from HA and HA does not re-subscribe on its own – the BTHome entities freeze until the ESP32 reboots
(re-upload the firmware or power-cycle) and HA reconnects. Use it only when HA is not connected, or reboot the
ESP32 afterwards."""
import asyncio, sys, time, collections
from aioesphomeapi import APIClient

async def main():
    host, secs = sys.argv[1], float(sys.argv[2])
    print("WARNING: this takes the proxy's advertisement subscription away from Home Assistant; reboot the ESP32 afterwards.")
    want = sys.argv[3].upper().replace(":", "") if len(sys.argv) > 3 else None
    c = APIClient(host, 6053, None)
    await c.connect(login=True)
    info = await c.device_info()
    print("device:", info.name, "bluetooth_proxy_feature_flags:", info.bluetooth_proxy_feature_flags)
    seen = collections.Counter(); samples = {}
    def cb(advs):
        for a in getattr(advs, "advertisements", advs):
            mac = f"{a.address:012X}"
            seen[mac] += 1
            if want is None or mac == want:
                samples.setdefault(mac, []).append((time.time(), a.rssi, a.address_type, bytes(a.data).hex()))
    states = []
    if hasattr(c, "subscribe_bluetooth_scanner_state"):
        c.subscribe_bluetooth_scanner_state(lambda st: states.append(st))
    c.subscribe_bluetooth_le_raw_advertisements(cb)
    await asyncio.sleep(secs)
    for st in states[-3:]:
        print("scanner state:", st)
    ents, _ = await c.list_entities_services()
    print(f"{len(ents)} entities exposed to Home Assistant")
    print(f"{sum(seen.values())} raw advertisements from {len(seen)} addresses in {secs:.0f} s")
    for mac, n in seen.most_common(8):
        print(f"  {':'.join(mac[i:i+2] for i in range(0,12,2))} x{n}")
    if want:
        v = samples.get(want, [])
        print(f"{want}: {len(v)} frames")
        for t, rssi, at, d in v[:6]:
            print(f"  {time.strftime('%H:%M:%S', time.localtime(t))} rssi={rssi} addr_type={at} data={d}")
    await c.disconnect()

asyncio.run(main())
