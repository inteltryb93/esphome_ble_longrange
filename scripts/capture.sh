#!/bin/bash
# Capture the ESPHome API log for N seconds into logs/<name>.log, then summarise it.
# Usage: capture.sh <seconds> <name> [host]
set -e
cd "$(dirname "$0")/.."
SECS="${1:-180}"; NAME="${2:-run}"; HOST="${3:-192.168.0.102}"
timeout "$SECS" /home/mateusz/xiaomi_esp_flasher/.venv/bin/esphome logs --device "$HOST" yaml/ble_longrange.yaml > "logs/$NAME.log" 2>&1 || true
python3 scripts/analyze_log.py "logs/$NAME.log"
