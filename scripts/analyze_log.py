#!/usr/bin/env python3
"""Summarise a ble_longrange log: per-device report timing (VV lines) and the periodic stats lines.
Usage: analyze_log.py <logfile> [mac-filter]"""
import re, sys, collections
rx = re.compile(r'\[(\d\d:\d\d:\d\d\.\d+)\]\[VV\]\[ble_lr\.scan:\d+\]: \[([0-9A-F:]{17})\] evt=0x(..) (\w+)( scan_rsp)? phy=(\w+)/(\w+) sid=(\d+) rssi=(-?\d+) .*len=(\d+) data=(\S+)')
per = collections.defaultdict(list)
stats = []
for line in open(sys.argv[1], errors="replace"):
    if "ble_lr:" in line and "stats:" in line:
        stats.append(line.strip()[:400]); continue
    m = rx.search(line)
    if not m: continue
    h, mi, s = m.group(1).split(':'); ts = int(h)*3600 + int(mi)*60 + float(s)
    per[m.group(2)].append(dict(ts=ts, evt=int(m.group(3), 16), phy=m.group(6), rssi=int(m.group(9)), data=m.group(11)))
if not per:
    print("no VV reports in log"); 
else:
    t0 = min(v[0]['ts'] for v in per.values()); t1 = max(v[-1]['ts'] for v in per.values())
    print(f"reports window {t1-t0:.0f}s, {len(per)} addresses")
    flt = sys.argv[2].upper() if len(sys.argv) > 2 else None
    for mac, v in sorted(per.items(), key=lambda kv: -len(kv[1])):
        if flt and flt not in mac: continue
        adv = [x for x in v if not (x['evt'] & 0x08)]
        gaps = [round(b['ts']-a['ts'], 1) for a, b in zip(adv, adv[1:])]
        cnt = collections.Counter(gaps)
        rssi = [x['rssi'] for x in v]
        print(f"{mac} phy={v[0]['phy']:5s} n={len(v):4d} adv={len(adv):4d} uniq={len(set(x['data'] for x in adv)):4d} "
              f"rssi min/avg/max={min(rssi)}/{sum(rssi)//len(rssi)}/{max(rssi)} rate={len(adv)*60/(t1-t0):.1f}/min "
              f"gaps={sorted(cnt.items())[:8]}")
print("---- stats lines ----")
for s in stats[-6:]:
    print(s)
