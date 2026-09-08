#!/usr/bin/env python3
"""List entities and current states through the ESPHome native API (what Home Assistant sees).
Usage: ha_entities.py [host] [seconds]"""
import asyncio, sys
from aioesphomeapi import APIClient

async def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "esp-ble-longrange.local"
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 5
    c = APIClient(host, 6053, None)
    await c.connect(login=True)
    info = await c.device_info()
    print("device:", info.name, info.esphome_version, info.model, "mac", info.mac_address)
    ents, _ = await c.list_entities_services()
    byid = {e.key: e for e in ents}
    print(f"---- {len(ents)} entities ----")
    for e in ents:
        print(f"{type(e).__name__:22s} {e.object_id:40s} {e.name}")
    states = {}
    def cb(s): states[s.key] = s
    c.subscribe_states(cb)
    await asyncio.sleep(wait)
    print("---- states ----")
    for e in ents:
        s = states.get(e.key)
        val = getattr(s, "state", None) if s is not None else "(no state yet)"
        if s is not None and getattr(s, "missing_state", False):
            val = "(unknown)"
        print(f"{e.name:40s} {val}")
    await c.disconnect()

asyncio.run(main())
