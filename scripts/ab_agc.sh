#!/bin/bash
# A/B: controller AGC-recorrect variant vs plain, alternating; each run = OTA flash, USB capture, one no-WiFi test.
# Usage: ab_agc.sh <config-index> <seconds> <variant...>   e.g. ab_agc.sh 8 300 noagc agc noagc agc
cd "$(dirname "$0")/.."
ESPHOME=/home/mateusz/xiaomi_esp_flasher/.venv/bin/esphome
PY=/home/mateusz/xiaomi_esp_flasher/.venv/bin/python
CFG="$1"; SECS="$2"; shift 2
n=0
for v in "$@"; do
  n=$((n+1))
  case "$v" in agc) Y=yaml/test_nowifi_sweep_agc.yaml;; *) Y=yaml/test_nowifi_sweep.yaml;; esac
  echo "$(date +%T) run $n: $v ($Y)"
  $ESPHOME run --no-logs --device 192.168.0.102 "$Y" 2>&1 | tr '\r' '\n' | grep -E "OTA successful|error:" | tail -1
  sleep 6
  P=$(/home/mateusz/xiaomi_esp_flasher/scripts/wait_port.sh 120) || { echo "no port"; continue; }
  LOG="logs/ab_${n}_${v}.log"
  timeout $((SECS+150)) $ESPHOME logs --device "$P" "$Y" > "$LOG" 2>&1 &
  CAP=$!
  sleep 20
  $PY scripts/nowifi_sweep.py 192.168.0.102 "$SECS" "$CFG" 2>&1 | grep -E "started|sweep done|not reachable"
  wait $CAP 2>/dev/null
  $PY scripts/nowifi_analyze.py "$LOG"
done
echo "AB DONE $(date +%T)"
