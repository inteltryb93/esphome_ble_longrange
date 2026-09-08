#!/bin/bash
# Poll the diagnostic entities over the native API every 5 min for N minutes (light traffic), one line per poll.
cd "$(dirname "$0")/.."
MIN="${1:-30}"; HOST="${2:-192.168.0.102}"
END=$(( $(date +%s) + MIN*60 ))
while [ $(date +%s) -lt $END ]; do
  L=$(/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/ha_entities.py "$HOST" 3 2>&1 | grep -E "^(Living Room (Temperature|Humidity|RSSI|Packet Counter|Battery Voltage)|BLE Reports|Free Heap|Scanner State) " | awk '{printf "%s=%s ", $1$2$3, $NF}')
  echo "$(date +%T) $L"
  sleep 300
done
echo "poll done"
