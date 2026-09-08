# esphome_ble_longrange

ESPHome external component `ble_longrange_scanner`: **BLE 5.0 extended scanning (LE 1M + LE Coded PHY / Long
Range)** on ESP32-C3 (Bluedroid, ESP-IDF 5.5.5). Xiaomi LYWSD03MMC thermometers running the pvvx firmware in
"BT5+ PHY + LE Long Range" mode (Advertising Extensions, Coded PHY S=8) become visible again and are decoded into
Home Assistant entities (temperature, humidity, battery, voltage, RSSI, PHY, "Long Range"). The stock
`esp32_ble_tracker` (legacy scan) cannot see such thermometers. Legacy advertisers are received by the same scan
(PHY 1M) and handed to `esp32_ble_tracker` / `bluetooth_proxy`.

```
components/ble_longrange_scanner/        the component (Python + C++), advertisement parser (BTHome v2 / pvvx / atc1441 / Mi)
examples/bluetooth_proxy_longrange.yaml  example: plain Bluetooth proxy (advertisements only) + Long Range (source: github)
examples/longrange_sensors.yaml          example: per-thermometer entities, statistics, api actions for tuning
yaml/bluetooth_proxy_lr.yaml             my lab config (source: local) – same as the proxy example
yaml/ble_longrange.yaml                  my diagnostic lab config (source: local)
docs/research.md                         findings from the ESPHome / ESP-IDF / pvvx sources (with line references)
docs/architecture.md                     what is reused from ESPHome, what was written from scratch, data flow, limits
docs/test_report.md                      results on real hardware (logs in logs/)
docs/facts.md, docs/test_plan.md         collected source facts (pvvx / ESP-IDF / ESPHome) and the test plan
scripts/capture.sh                       capture the API log for N s + summary (scripts/analyze_log.py)
scripts/ha_entities.py                   list entities and states over the native API (what HA sees)
scripts/sweep.py, sweep_coded.py         runtime scan-parameter sweep (api actions set_scan / set_coex), 1M+Coded / Coded only
scripts/proxy_check.py                   bluetooth_proxy raw-advertisement stream (what HA's Bluetooth integration gets)
scripts/stability_poll.sh                poll the diagnostic entities every 5 min (stability test)
scripts/nowifi_sweep.py, nowifi_analyze.py, ab_agc.sh   no-WiFi measurements over USB serial
scripts/set_longrange.sh                 enable LR through the flasher API (thermometer in legacy mode) / reset_longrange.sh: revert
```

## How it works (short)

* ESPHome never queues `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` and its GAP handler is protected. The component takes the
  current callback (`esp_ble_gap_get_callback()`), registers its own and **chains** everything it does not handle
  itself to ESPHome – no changes to the ESPHome checkout.
* In the BT task each report is copied into a pool (23 × 268 B) and a lock-free queue; the main loop decodes,
  publishes entities and (optionally) hands the frame to the tracker as a `BLEScanResult`
  (`gap_scan_event_handler`), which is what `bluetooth_proxy` and every ESPHome BLE sensor platform consume.
* One continuous extended scan (`esp_ble_gap_set_ext_scan_params` with mask 1M|Coded,
  `esp_ble_gap_start_ext_scan(0,0)`), a no-report watchdog, retries with backoff, fragment reassembly and
  legacy ADV+SCAN_RSP merging.
* sdkconfig: `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_EXTEND_SCAN_EN=y` (ESPHome turns 5.0 off by
  default); BLE 4.2 stays enabled so the tracker/client still compile.

## Configuration – Bluetooth proxy mode (`examples/bluetooth_proxy_longrange.yaml`)

Exactly like a regular ESPHome proxy (`esp32_ble_tracker` + `bluetooth_proxy: active: false`) plus the
`ble_longrange_scanner:` block:

```yaml
external_components:
  - source: github://inteltryb93/esphome_ble_longrange
    components: [ble_longrange_scanner]

esp32:
  board: esp32-c3-devkitm-1
  framework: {type: esp-idf}

esp32_ble_tracker:
  scan_parameters:
    active: true

bluetooth_proxy:
  active: false               # advertisements only (GATT connections cannot run next to the extended scan)

ble_longrange_scanner:        # replaces the tracker's legacy scan with an extended scan on 1M + Coded PHY
  scan_parameters:
    interval: 400ms
    window: 20ms              # 1M: small window, legacy devices stay visible
    coded_interval: 400ms
    coded_window: 380ms       # Coded: window ≈ interval (measured optimum)
```

Requirements: ESP32-C3/S3/C6/C5/H2 (BLE 5.0 controller; the classic ESP32 and the S2 are BLE 4.2 only – the
configuration is rejected with a clear error), framework `esp-idf`, ESPHome 2026.1+.

In Home Assistant the device looks and behaves like any Bluetooth proxy: no entities, scanner state RUNNING,
passive/active mode driven from HA, advertisements (legacy and Coded PHY) delivered to the Bluetooth / BTHome
integrations. The tracker's `esp_ble_gap_set_scan_params/start_scanning/stop_scanning` calls are redirected at
link time (`-Wl,--wrap`) to the component, which emulates the completion events – the tracker "thinks" it scans
while the extended scan is the only real one (the C3 controller does not allow both at once).

Component options (all optional):

```yaml
ble_longrange_scanner:
  scan_parameters:
    interval: 400ms           # LE 1M
    window: 80ms
    active: true              # overridden by the tracker/HA mode (proxy active: false -> passive)
    coded_interval: 400ms     # LE Coded
    coded_window: 300ms
    phy: both                 # both | 1m | coded  – "coded": Long Range only, no legacy devices
  report_timeout: 120s        # no reports -> restart the scan
  stats_interval: 60s         # statistics line in the log (INFO)
  forward_to_tracker: true    # reports -> tracker -> bluetooth_proxy / BLE sensor platforms
  coex_prefer_bt: true        # esp_coex_preference_set(ESP_COEX_PREFER_BT): +60 % Coded reception with WiFi on
  log_unknown_devices: false
  # diagnostic variant (examples/longrange_sensors.yaml): per-thermometer entities and statistics
  reports_1m: {name: BLE Reports 1M}
  reports_coded: {name: BLE Reports Coded}
  free_heap: {name: Free Heap}
  scanner_state: {name: Scanner State}
  scanning: {name: Scanning}
  devices:
    - mac_address: "A4:C1:38:4A:E8:8C"
      name: Living Room       # entities: <name> Temperature/Humidity/Battery/Battery Voltage/RSSI/Packet Counter,
                              #           <name> PHY, <name> Advertising Format (text), <name> Long Range (binary)
      packet_counter: false   # any entity can be disabled (false) or overridden (name/filters/...)
```

## Build / flash / tests

```bash
ESPHOME=/home/mateusz/xiaomi_esp_flasher/.venv/bin/esphome        # editable install of the 2026.10.0-dev checkout
cd /home/mateusz/esphome_ble_longrange
$ESPHOME compile yaml/bluetooth_proxy_lr.yaml
$ESPHOME run --no-logs --device 192.168.0.102 yaml/bluetooth_proxy_lr.yaml   # OTA (or --device /dev/ttyACM1)
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/proxy_check.py 192.168.0.102 120 A4:C1:38:4A:E8:8C
scripts/capture.sh 300 run                                              # VERY_VERBOSE log for 5 min + statistics
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/ha_entities.py 192.168.0.102 10
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/sweep.py 192.168.0.102 3   # scan-window tuning
```

Back to the flasher firmware: `cd /home/mateusz/xiaomi_esp_flasher && scripts/flash_esp.sh`.

**Note (applies to every ESPHome proxy):** `bluetooth_proxy` has a single advertisement subscriber slot – "newest
wins". `scripts/proxy_check.py` (or any other `aioesphomeapi` client subscribing to advertisements) takes it away
from Home Assistant and HA does not re-subscribe by itself: the BTHome entities freeze until the ESP32 reboots
(re-upload the firmware / power cycle). Do not run that script while HA is connected, or reboot the ESP32 afterwards.

## Home Assistant

The device `bluetooth-proxy-lr` (API, mDNS) shows up in HA as a discovered ESPHome integration: *Settings →
Devices & services → Discovered → ESPHome "bluetooth-proxy-lr" → Configure*. Once added, HA uses it as a
Bluetooth proxy: the Long Range thermometer appears in the BTHome integration like any other (identical BTHome v2
data, public address).

## Reverting a thermometer from Long Range to legacy

The LR flag is not persisted: **removing and re-inserting the battery** restores BT 4.2 (pvvx README line 138).
Alternatively command `0xDD` (CMD_ID_LR_RESET) or `0x56` (defaults) on characteristic 0x1F1F over a Coded-PHY
connection (nRF Connect on a BT5 phone; this component does not open connections). Afterwards
`scripts/set_longrange.sh <flasher-ip> <MAC>` enables LR again (requires the flasher firmware on the ESP32).

## Long Range reception – what limits it

Measured (`docs/test_report.md` §5, §8d, §8e): with a good signal (thermometer close, WiFi off) the firmware
receives **98–100 %** of the Coded advertising events – host, queue and Bluedroid lose nothing. With a weak signal
(−85…−95 dBm) reception drops to 50–70 % without WiFi and 18–30 % with WiFi + HA: the losses are fades below the
Coded-S8 sensitivity (−104 dBm) and radio time taken by WiFi. The scan parameters are already optimal (Coded
window = interval, 1M 20 ms or `phy: coded`); controller options (AGC recorrect, flow control, TLIM) do not change
the result. What really helps: better ESP32 placement/antenna, higher thermometer TX power (pvvx `rf_tx_power`,
up to +10 dBm), less WiFi traffic, a second proxy closer to the thermometer.

Measurements, limitations and reception statistics: `docs/test_report.md`.
