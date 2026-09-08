#!/usr/bin/env python3
"""Count the Long Range thermometer's Coded reports between TEST START/END markers in a serial log.
Usage: nowifi_analyze.py <logfile> [MAC] [adv-interval-s]"""
import re, sys
log = sys.argv[1]; mac = (sys.argv[2] if len(sys.argv) > 2 else "A4:C1:38:4A:E8:8C").upper()
adv = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
ts = lambda m: int(m.group(1))*3600 + int(m.group(2))*60 + float(m.group(3))
rx_t = re.compile(r'\[(\d\d):(\d\d):(\d\d\.\d+)\]')
rx_hit = re.compile(re.escape(f"[{mac}] evt=0x01 ext phy=Coded"))
rx_all = re.compile(r'evt=0x01 ext phy=Coded')
cur = None; res = []
for line in open(log, errors="replace"):
    t = rx_t.search(line)
    if not t: continue
    if "TEST START" in line:
        cur = {"label": line.split("TEST START",1)[1].split("(")[0].strip(), "t0": ts(t), "n": 0, "all": 0}
    elif "TEST END" in line and cur:
        cur["t1"] = ts(t); res.append(cur); cur = None
    elif cur:
        if rx_hit.search(line): cur["n"] += 1
        if rx_all.search(line): cur["all"] += 1
for r in res:
    d = r["t1"] - r["t0"]; exp = d / adv
    print(f"{r['label']:34s} {d:5.0f}s  {mac}: {r['n']:3d} reports = {r['n']*60/d:5.1f}/min = {r['n']/exp*100:4.0f}%   (all Coded advertisers: {r['all']})")
