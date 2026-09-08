#!/bin/bash
# Enable BT5 PHY + LE Long Range on a pvvx thermometer through the xiaomi_esp_flasher API (device must be in legacy mode).
# Usage: set_longrange.sh <esp-ip> <MAC>
# Sends the current 55 config back with flg2 |= 0x60 (bt5phy | longrange). The device reboots into LR on disconnect.
set -e
H="${1:?esp ip}"; MAC="${2:?mac}"
curl -s -H "Content-Length: 0" -X POST "http://$H/api/device/$MAC/connect" >/dev/null
for i in $(seq 1 40); do sleep 2; hd=$(curl -s "http://$H/api/status" | python3 -c "import sys,json;print(json.load(sys.stdin).get('hold_device',''))"); [ "$hd" = "$MAC" ] && break; done
[ "$hd" = "$MAC" ] || { echo "connect failed"; exit 1; }
cfg=$(curl -s -X POST -H 'Content-Type: application/json' -d '{"hex":"55","wait":2500}' "http://$H/api/device/$MAC/cmd" | python3 -c "import sys,json;n=[x['hex'] for x in json.load(sys.stdin)['notify'] if x['hex'].startswith('55')];print(n[0] if n else '')")
[ -n "$cfg" ] || { echo "no 55 answer"; exit 1; }
echo "current: $cfg"
new=$(python3 -c "
h='$cfg'; b=bytearray.fromhex(h)
# 55 ver flg flg2 ... -> write without ver: 55 flg flg2|0x60 rest
out=bytearray([0x55,b[2],b[3]|0x60])+b[4:13]
print(out.hex())")
echo "sending: $new"
curl -s -X POST -H 'Content-Type: application/json' -d "{\"hex\":\"$new\",\"wait\":2500}" "http://$H/api/device/$MAC/cmd"; echo
curl -s -H "Content-Length: 0" -X POST "http://$H/api/device/$MAC/disconnect" >/dev/null
echo "disconnected – the thermometer reboots into Long Range now and disappears from legacy scanners."
echo "Recovery: pull the battery (LR flag resets on power loss) or send 0xDD over a Coded-PHY connection."
