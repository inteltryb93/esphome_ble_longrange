# Test report – real hardware, 2026-09-07

Gateway: ESP32-C3 (QFN32 rev 0.4, 4 MB flash, USB-Serial/JTAG), ESPHome 2026.10.0-dev (local checkout), ESP-IDF 5.5.5,
Bluedroid with `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y` + `CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`, WiFi 192.168.0.102,
build `yaml/ble_longrange.yaml`. Test device **A4:C1:38:4A:E8:8C** („Living Room”, LYWSD03MMC B1.7, pvvx 5.9,
BTHome v2) in **BT5 PHY + LE Long Range** mode since 22:09 (cfg flg2 0x70, adv interval 2.5 s, measure ×4 = 10 s).
Legacy references: A4:C1:38:0B:5F:4E (P2Kuchnia), A4:C1:38:B8:31:E8 (P3Sypialni) and 4 more LYWSD03MMC.
Logs: `logs/build1.log`, `logs/run1.log` (first boot), `logs/run2.log` (5 min VERY_VERBOSE), `logs/exp1_*.log`,
`logs/sweep1.txt`, `logs/stability.log`, `logs/proxy.log`.

## Summary

```
Build:            BLE 5.0 API compiled next to BLE 4.2 (tracker/client still link), flash 1 254 266 B (68.4 %),
                  RAM 143 656 B static; with bluetooth_proxy 1 266 746 B / 145 216 B
Ext scan:         SET_EXT_SCAN_PARAMS_COMPLETE status 0, EXT_SCAN_START_COMPLETE status 0, scan continuous
Long Range:       A4:C1:38:4A:E8:8C received on primary PHY = Coded, secondary PHY = Coded, SID 0, event_type 0x01
                  (extended, connectable, non-scannable), data complete, 29–30 B (flags + BTHome v2 + name ATC_4AE88C),
                  RSSI −76…−80 dBm
Decoding:         BTHome v2: 23.26 °C / 62.79 % / 86 % / 2.896 V, packet id incrementing by 1 every 10 s
                  (matches the last legacy readings 23.25 °C / 63.08 % / 2911 mV from 22:05 and the pvvx firmware:
                  the two BTHome frames alternate every 2.5 s, the pid increments per measurement)
Legacy:           the same extended scan delivers the legacy advertisers (PHY 1M, event_type 0x13/0x1b),
                  P2Kuchnia 22.84 °C / 55.30 % / 100 %, P3Sypialni 2.674 V … decoded and forwarded to the tracker
Home Assistant:   34 entities over the native API with live values, "Living Room Long Range: True",
                  "Living Room PHY: Coded", "Scanner State: RUNNING"
Reception rate:   5–8 of 24 Coded events/min (22–35 %) for every scan setting incl. Coded-only (§5) – the ceiling
                  is in the controller/advertiser channel behaviour, not in the host code (dropped=0)
bluetooth_proxy:  raw Coded-PHY frames of the thermometer delivered to the HA raw-advertisement stream (§8)
Stability:        36 min, no reset, restarts=0, dropped=0, min free heap 78 776 B, 5.7 Coded reports/min (§7)
Thermometer:      left in Long Range mode (that is the point – the data flows into Home Assistant); revert = battery pull / 0xDD
Final firmware:   bluetooth-proxy-lr (yaml/bluetooth_proxy_lr.yaml, §8b) – a plain advertisement-only Bluetooth proxy
                  with Long Range reception; the flasher firmware is NOT restored (restore: xiaomi_esp_flasher/scripts/flash_esp.sh)
```

## 1. test_build_ble50

`esphome compile yaml/ble_longrange.yaml` (`logs/build1.log`):

```
sdkconfig: CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y  CONFIG_BT_BLE_50_EXTEND_SCAN_EN=y
           CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y  CONFIG_BT_BLE_42_SCAN_EN=y
RAM:   [====      ]  44.7% (used 143528 bytes from 321296 bytes)
Flash: [=======   ]  68.0% (used 1247192 bytes from 1835008 bytes)        (first build, no bluetooth_proxy)
Flash: 1 254 266 B (68.4 %) with the api actions; 1 266 746 B (69.0 %) / RAM 145 216 B with bluetooth_proxy
```

ESPHome's own reconcile step (`esp32/__init__.py:2322`) defaults BLE 5.0 to off; the component sets the option
from `to_code` and the value wins (verified in `yaml/.esphome/build/esp-ble-longrange/sdkconfig.esp-ble-longrange`).
No compiler warnings from the component. **PASS**

## 2. test_ext_scan_start

First boot (`logs/run1.log`, 22:41:29) and every restart since:

```
[D][ble_lr]: GAP callback hooked (chaining to esp32_ble handler 0x4200c1f6)
[I][ble_lr.scan]: Starting extended scan: 1M active 400.0/80.0 ms, Coded passive 400.0/300.0 ms, no duplicate filter
[D][ble_lr.scan]: SET_EXT_SCAN_PARAMS_COMPLETE status 0
[D][ble_lr.scan]: EXT_SCAN_START_COMPLETE status 0
[I][ble_lr.scan]: Extended scan running (1M + Coded PHY)
```

`esp32_ble` never logged „Ignoring unexpected GAP event type”: the hook receives all extended-scan events and
chains the rest to ESPHome. A bug found on this first run (the no-report watchdog compared unsigned timestamps
and restarted the scan every few ms – `restarts=1069` in the first stats line) was fixed (signed difference) and
re-flashed; since then `restarts=0 failures=0`. **PASS**

## 3. test_legacy_still_visible

`logs/run2.log` (5 min, VERY_VERBOSE, `scripts/analyze_log.py`): 16 addresses on PHY 1M, 1118 legacy reports
(≈ 220/min), 322 scan responses merged with their ADV_IND. My other thermometers in legacy mode:

| MAC | PHY | reports / 5 min | RSSI | decoded |
|---|---|---|---|---|
| A4:C1:38:0B:5F:4E P2Kuchnia | 1M | 6 | −91…−93 | BTHome v2 22.84 °C / 55.30 % / 100 % |
| A4:C1:38:B8:31:E8 P3Sypialni | 1M | 4 | −92…−94 | BTHome v2 2.674 V frame / 21.65 °C 56.76 % 59 % |
| A4:C1:38:93:72:DB | 1M | 3 | −91…−93 | BTHome v2 (not configured, logged as unconfigured) |
| A4:C1:38:B0:E4:4A, AA:01:81, 3D:A8:7F | 1M | 1–2 | −92…−99 | BTHome v2 |

They are far from the gateway (RSSI −91…−99 dBm, 1M window 80 ms of 400 ms), hence the low counts – the flasher's
legacy scan (320/60 ms) saw them at a similar rate (`last_seen_ago` 19–106 s at 22:18). Every legacy report ≤ 62 B was
also forwarded to `esp32_ble_tracker` (`fwd=822` of 1145 in 5 min; `too_long=0`). **PASS**

## 4. test_lr_switch

Done at 22:09 from the flasher's Configure tab (`Send 5587700a062804a9313100b4`, cfg flg2 0x70). Confirmed at 22:18 before flashing: the flasher (legacy scan)
reported the device `offline`, last seen 22:05:11, and its connect attempt failed (`BLE_TIMEOUT`, reason 133); the
PC adapter (Intel 8087:0a2b, HCI 4.2) cannot receive Coded PHY at all. **PASS (pre-condition verified)**

## 5. test_lr_report + test_lr_decode

First Coded-PHY report 1 s after the scan started (`logs/run2.log`):

```
[22:44:14.503][VV][ble_lr.scan]: [A4:C1:38:4A:E8:8C] evt=0x01 ext phy=Coded/Coded sid=0 rssi=-76 tx=127 status=0 len=29
   data=02.01.06.0D.16.D2.FC.40.00.D2.0C.50.0B.10.00.11.01.0B.09.41.54.43.5F.34.41.45.38.38.43
[22:44:14.507][D][ble_lr.dev]: [A4:C1:38:4A:E8:8C] Living Room: phy=Coded/Coded ext rssi=-76 fmt=BTHome v2 ... 2896 mV cnt=210 name='ATC_4AE88C'
[22:44:42.024][VV][ble_lr.scan]: [A4:C1:38:4A:E8:8C] evt=0x01 ext phy=Coded/Coded sid=0 rssi=-78 tx=127 status=0 len=30
   data=02.01.06.0E.16.D2.FC.40.00.D5.01.56.02.16.09.03.87.18.0B.09.41.54.43.5F.34.41.45.38.38.43
[22:44:42.032][D][ble_lr.dev]: [A4:C1:38:4A:E8:8C] Living Room: phy=Coded/Coded ext rssi=-78 fmt=BTHome v2 T=23.26°C H=62.79% batt=86% cnt=213
```

Frame A (`40 00 pid 01 batt 02 temp 03 hum`) = 23.26 °C / 62.79 % / 86 %; frame B (`40 00 pid 0C vbat 10 00 11 01`) =
2.896 V. Flags 0x06, complete name `ATC_4AE88C` inside the AUX_ADV_IND (as `load_adv_data()` does in Coded mode).
Values are consistent with the last legacy readings before the switch (23.25 °C / 63.08 % / 2911 mV at 22:05) and
with the neighbouring P2Kuchnia (22.84 °C). The pid advanced 210 → 216 in 60 s (one per 10 s measurement, as in
`app_advertise_prepare_handler()`), later 223 → 45 (8-bit wrap) in 10 min. **PASS**

Reception statistics (5 min, 1M 400/80 ms active, Coded 400/300 ms passive, VV log streamed over WiFi):

```
A4:C1:38:4A:E8:8C phy=Coded n=26 adv=26 uniq=23 rssi min/avg/max=-79/-78/-76 rate=5.2/min
gaps (s): 1.5×1, 5.0×7, 7.5×1, 8.5×1, 10.0×4, 12.5×3, 15.0×1, 17.5×5
```

The thermometer transmits one extended advertising event every 2.5 s (24/min); the gateway received 5.2/min ≈
**22 %** of them. Parameter sweep (`scripts/sweep.py`, api actions, states-only API traffic, 3 min per config,
`logs/sweep1.txt`):

| # | 1M (interval/window ms) | Coded (interval/window ms) | coex | Coded reports/min | 1M reports/min |
|---|---|---|---|---|---|
| 0 | 400/80 act | 400/300 | balanced | 4.0 | 206.6 |
| 1 | 400/30 act | 400/370 | balanced | 4.3 | 90.0 |
| 2 | 1000/50 act | 1000/950 | balanced | 5.7 | 65.0 |
| 3 | 400/30 act | 100/95 | balanced | 7.0 | 40.7 |
| 4 | 400/30 act | 400/370 | BT | 7.0 | 100.7 |
| 5 | 400/30 passive | 400/370 | balanced | 5.0 | 54.0 |
| 6 | 4000/30 act | 4000/3900 | balanced | 6.0 | 5.7 |
| 7 | 400/30 act | 400/400 | balanced | 6.0 | 85.0 |

The thermometer sends 24 events/min; the best 1M+Coded combinations reach 7/min (29 %). Reducing the 1M share
(passive, tiny window) does not help, a bigger Coded window does not scale linearly, `coex_prefer_bt` gives +60 %
at identical windows: the radio time lost to WiFi (and the 1M scan) is the limiting factor, not the host.



Coded-only sweep (`phy_mask` 2, `scripts/sweep_coded.py`, `logs/sweep2_coded.txt`, 3 min per config):

| # | PHYs | Coded (interval/window ms) | coex | Coded reports/min |
|---|---|---|---|---|
| 0 | Coded only | 100/95 | balanced | 5.7 |
| 1 | Coded only | 400/400 | balanced | 6.0 |
| 2 | Coded only | 100/100 | balanced | 7.0 |
| 3 | Coded only | 30/30 | balanced | **8.3** |
| 4 | Coded only | 100/95 | BT | 6.3 |
| 5 | Coded only | 1000/1000 | balanced | 7.0 |
| 6 | 1M 400/30 + Coded | 100/95 | balanced | 5.3 |
| 7 | Coded only | 50/50 | balanced | 5.3 |

Dropping the 1M scan entirely does not change the picture: 5–8/min ≈ 22–35 % with a 100 %
Coded duty cycle. The ceiling of ≈ 1/3 is consistent with the scanner listening on one primary channel per interval
while the advertiser's ADV_EXT_IND (Coded S8) is effectively caught on one of the three channels per event; the
host never drops anything (`dropped=0`, queue 23 deep). Final configuration: `phy: both`, 1M 400/60 ms active,
Coded 400/370 ms, `coex_prefer_bt: true` (the legacy thermometers stay visible; `phy: coded` is available when only
Long Range devices matter).

## 6. test_ha_entities

`scripts/ha_entities.py 192.168.0.102` (aioesphomeapi, what Home Assistant receives), 22:47:

```
device: esp-ble-longrange 2026.10.0-dev esp32-c3-devkitm-1
Living Room Temperature     23.24 °C        Living Room Long Range        True
Living Room Humidity        62.83 %         Living Room PHY               Coded
Living Room Battery         87 %            Living Room Advertising Format BTHome v2
Living Room Battery Voltage 2.896 V (*)     Living Room RSSI              -78 dBm
Living Room Packet Counter  223             Scanner State                 RUNNING   Scanning True
P2Kuchnia Temperature 22.81 °C  Humidity 55.40 %  Battery 100 %  RSSI -92  PHY 1M  Long Range False
P3Sypialni Temperature 21.65 °C Humidity 56.76 %  Battery 59 %   RSSI -92  PHY 1M  Long Range False
BLE Reports 1M 221/min   BLE Reports Coded 4/min   Free Heap 101 460 B
```

(*) the voltage-only BTHome frame was first rejected by the parser as "no measurement" (`valid` required
temperature/humidity/battery %); fixed – voltage counts as a measurement, verified in the stability run.
Entity list: 3 devices × (6 sensors + 2 text sensors + 1 binary sensor) + 3 global sensors + 1 text + 1 binary =
32 entities, plus `api` actions `set_scan`, `set_coex`, `restart_scan`. **PASS**

Home Assistant runs at http://192.168.0.49/ (HA 2026.7.2); it connected to the device on its own once the
proxy-mode firmware came up (§8b).

## 7. test_stability_30min

Final configuration flashed 00:16:37 (`logs/final_boot.log`), then 36 minutes without touching the device: the
diagnostic entities were polled over the native API every 5 min (`scripts/stability_poll.sh`,
`logs/stability_poll.log`), `bluetooth_proxy` was exercised for 2×150 s, and a final 150 s VERY_VERBOSE capture
(`logs/stability.log`) read the cumulative counters:

```
00:20:15  pid=20   1M=172/min coded=2/min  heap=99280  RUNNING
00:25:18  pid=51   1M=173/min coded=5/min  heap=99664  RUNNING
00:30:23  pid=81   1M=186/min coded=6/min  heap=99852  RUNNING
00:35:26  pid=111  1M=141/min coded=7/min  heap=99852  RUNNING
00:40:30  pid=141  1M=153/min coded=5/min  heap=99852  RUNNING
00:45:34  pid=172  1M=163/min coded=4/min  heap=99852  RUNNING
00:52:37 stats: state=RUNNING scan_uptime=2159s total=6008 1M=5804 coded=204 (5.0/min) legacy=5804 ext=204
         scan_rsp=1871 frag=0/0 fwd=4136 too_long=0 dropped=0 restarts=0 failures=0 heap=99196 min_heap=78776
```

* No reset (`scan_uptime` 2159 s = since the flash, `Scanner State` RUNNING at every poll, no `[W]`/`[E]` lines
  except the logger's own VERY_VERBOSE notice), `restarts=0`, `failures=0`, `dropped=0`.
* Heap: 99–100 KB free at rest, minimum 78 776 B (during the API log streams / OTA), well above the 30 KB limit.
* Reception: 204 Coded reports in 2159 s = **5.7/min = 24 % of the 24 events/min** the thermometer transmits; the
  thermometer's packet id advanced 20 → 172 (one per 10 s), i.e. the measurements themselves were never missed for
  longer than a few tens of seconds (largest gap in the final capture 32.5 s). Living Room temperature drifted
  23.10 → 23.11 °C, humidity 62.12 → 61.95 %, voltage 2.895 V constant.
* Legacy: 5804 1M reports (150–185/min) from ~15 addresses, 4136 forwarded to the tracker/proxy.
  **PASS** (stability); reception target ≥ 80 % **not reached** – hardware ceiling, see §5.

## 8. bluetooth_proxy (phase 2)

`bluetooth_proxy: active: false` (advertisement-only; 1 266 746 B flash, 145 216 B RAM). `scripts/proxy_check.py`
subscribes to the same raw-advertisement stream Home Assistant's Bluetooth integration uses
(`subscribe_bluetooth_le_raw_advertisements`, `bluetooth_proxy_feature_flags: 97`), 120 s, `logs/proxy.log`:

```
225 raw advertisements from 10 addresses in 120 s   (28:6B:B4:9F:3F:1F x87, ..., A4:C1:38:4A:E8:8C x12, A4:C1:38:AA:01:81 x2, ...)
A4C1384AE88C: 12 frames
  00:20:22 rssi=-79 addr_type=0 data=0201060e16d2fc40001501560208090343180b094154435f344145383843
  00:20:30 rssi=-78 addr_type=0 data=0201060d16d2fc4000160c4f0b100011010b094154435f344145383843
```

The Coded-PHY frames of the Long Range thermometer reach HA exactly like legacy ones: public address (type 0),
RSSI and the complete AD payload (flags, BTHome v2 service data 0xFCD2, name) – HA's BTHome integration needs
nothing else (it does not know or care about the PHY). Legacy advertisers (e.g. A4:C1:38:AA:01:81) arrive through
the same path with their scan responses merged. **PASS** (the HA side still has to add the ESPHome device – the
proxy stream starts when HA subscribes).

## 8b. Bluetooth-proxy mode (`yaml/bluetooth_proxy_lr.yaml`, flashed 01:15, the final firmware on the ESP32)

The deliverable is a plain Bluetooth proxy: standard `esp32_ble_tracker` +
`bluetooth_proxy: active: false` YAML plus `ble_longrange_scanner:`. Experiment `logs/exp_tracker_scan.log`
(tracker legacy scan allowed to run): the tracker's `SCAN_PARAM_SET/START_COMPLETE` succeed and then the
extended scan is refused (`set ext scan params failed (status 12)`) – legacy and extended scans exclude each
other on the ESP32-C3 controller in either order. Therefore the tracker's three scan API calls are wrapped at
link time and emulated (see `docs/architecture.md`). Result (`logs/proxy_lr_boot.log`, `logs/proxy_lr_check.log`):

```
[C][esp32_ble_tracker]:   Scanner State: RUNNING
[C][ble_lr]:   esp32_ble_tracker: legacy scan emulated, reports forwarded: YES
[I][ble_lr.scan]: Tracker requests passive scanning – applying to the 1M PHY      (bluetooth_proxy active: false)
[D][ble_lr.scan]: Tracker scan emulated: period 300 s (extended scan keeps running)
[I][ble_lr]: stats: state=RUNNING scan_uptime=48s total=155 1M=147 coded=8 (8.0/min) fwd=144 dropped=0
scanner state: BluetoothScannerStateResponse(state=RUNNING, mode=PASSIVE, configured_mode=ACTIVE)
0 entities exposed to Home Assistant
337 raw advertisements from 10 addresses in 120 s   (A4:C1:38:4A:E8:8C x9, A4:C1:38:B8:31:E8 x3, A4:C1:38:0B:5F:4E x2, ...)
A4C1384AE88C: 01:17:38 rssi=-77 addr_type=0 data=0201060d16d2fc40006e0c4f0b100011010b094154435f344145383843
```

Home Assistant itself connected to the new firmware within a second of the boot
(`[api.connection]: Home Assistant 2026.7.2 (192.168.0.49): connected`, then `[bluetooth_proxy]: Setting scanner
mode to passive` – HA drives the mode exactly as with any proxy). After 9 minutes (`logs/proxy_lr_stability.log`):
`state=RUNNING scan_uptime=543s total=2377 1M=2307 coded=70 (7.7/min avg) fwd=2338 dropped=0 failures=0
heap=102420 min_heap=83924 coded_rssi=[-78..-73]`; the 3 extended-scan restarts are the mode switches requested
through the proxy (each costs ~0.5 s of scanning).

Flash 1 245 414 B (67.9 %), RAM 143 912 B. HA sees a proxy with no entities, scanner RUNNING/PASSIVE, and the
Long Range thermometer's BTHome frames in the raw stream (4.5/min in this 2-min sample, RSSI −74…−77). **PASS**

## 8c. Incident: BTHome entities frozen in Home Assistant (01:17–01:36) – root cause and fix

`ATC E88C Packet Id` in Home Assistant had not updated for 13 minutes. Read-only check through the HA REST
API: every ATC E88C entity was last updated at 01:17:30, while the ESP32 kept receiving the thermometer
(`coded=70 → 90` between 01:29 and 01:34, `dropped=0`). 01:17:38 is exactly when `scripts/proxy_check.py`
subscribed to the proxy's raw-advertisement stream: an ESPHome `bluetooth_proxy` has a single advertisement
subscriber slot and the **newest subscriber wins** (`bluetooth_proxy.cpp: subscribe_api_connection`), so the test
script displaced Home Assistant, and HA did not re-subscribe after the script disconnected. This is standard
ESPHome proxy behaviour, not a fault of the component. Fix without touching HA: reboot the ESP32 (firmware
re-uploaded 01:35:40) – HA reconnected and re-subscribed; 60 s later `packet_id 228 updated 8 s ago`,
`temperature 23.09 updated 21 s ago` (voltage/battery keep their older timestamp because their values did not
change). `proxy_check.py` now prints a warning and README documents the rule: do not subscribe to the proxy stream
while HA is connected, or reboot the ESP32 afterwards. HA device: Xiaomi Temperature/Humidity sensor, BTHome,
A4:C1:38:4A:E8:8C (`/config/devices/device/8b58be413a3e68ce20214b77cac61d32`).

## 8d. A/B test: WiFi off vs on (USB serial, `yaml/test_nowifi.yaml`, `logs/nowifi_serial.log`, 02:06–02:19)

Same firmware and scan settings (1M 400/80 active→passive, Coded 400/300, coex BT), WiFi disabled at boot
(`enable_on_boot: false`) and enabled after 330 s by an `on_boot` automation; log over USB Serial/JTAG, only the
thermometer's own Coded reports counted (`[A4:C1:38:4A:E8:8C] evt=0x01 ext phy=Coded`):

| phase | duration | Coded reports | rate | share of the 24 events/min |
|---|---|---|---|---|
| A: WiFi off, no API client | 327 s | 67 | 12.3/min | **51 %** |
| B: WiFi on, Home Assistant connected and subscribed | 423 s | 31 | 4.4/min | **18 %** |

Legacy 1M reports show the same effect (500–630/min without WiFi vs 150–200/min with WiFi). WiFi coexistence on the
single ESP32-C3 radio is therefore the dominant limiter (factor ≈ 2.8); the remaining gap to 100 % in phase A is
the scanner/advertiser channel behaviour. Two extra extended-scan restarts at 02:16:31/02:16:46 were mode
switches requested by Home Assistant (auto active-scan windows through the proxy); each costs ~0.5 s.
The proxy firmware (WiFi on at boot) was re-flashed at the end of the test (02:25:43).

## 8e. Maximising Coded reception without WiFi (02:45–05:15, USB serial, `yaml/test_nowifi_sweep*.yaml`)

Method: WiFi on at boot, `set_scan` action applies the parameters, `nowifi_test` action disables WiFi for 300 s and
re-enables it; the log is captured over USB Serial/JTAG and only the thermometer's own Coded reports are counted
(`scripts/nowifi_sweep.py`, `scripts/nowifi_analyze.py`, `scripts/ab_agc.sh`; logs `logs/nowifi_sweep*_serial.log`,
`logs/ab_*.log`). Caveat: I moved the thermometer around during rounds 1–2 (received RSSI −75…−104 dBm, median
−88 dBm), so those rows are only indicative; from 04:21 the thermometer stood next to the receiver (−13 dBm).

| round | scan setting (no WiFi) | received of 120 events | share |
|---|---|---|---|
| 1 A | both PHYs, 1M 400/80 act, Coded 400/300 | 59 | 49 % |
| 1 B | Coded only 400/400 | 89 | 74 % |
| 1 C | Coded only 30/30 | 66 | 55 % |
| 1 E | both, 1M 400/30, Coded 400/370 | 67 | 56 % |
| 1 F | Coded only 1000/1000 | 72 | 60 % |
| 2 G | Coded only 200/200 | 74 | 62 % |
| 2 H | Coded only 300/300 | 81 | 67 % |
| 2 I | Coded only 400/400 (repeat) | 68 | 57 % |
| 2 J | Coded only 500/500 | 63 | 52 % |
| 2 K | Coded only 600/600 | 58 | 48 % |
| 2 L | both, 1M 400/20, Coded 400/380 | 68 | 57 % |
| 3 | Coded only 400/400, AGC recorrect, thermometer −86…−90 dBm | 64 | 53 % |
| 3 | Coded only 400/400, AGC recorrect, thermometer moved next to the receiver at 04:21 | 111 | 92 % (24/24 in the first full minute) |
| A/B 1 | Coded only 400/400, plain controller, −13 dBm | 119 | 99 % |
| A/B 2 | Coded only 400/400, AGC recorrect, −13 dBm | 121 | 100 % |
| A/B 3 | Coded only 400/400, plain controller, −13 dBm | 118 | 98 % |
| A/B 4 | Coded only 400/400, AGC recorrect, −13 dBm | 126 | 100 % (105 % – the advertiser sends slightly more than 24/min) |

Loss pattern at −85…−90 dBm is random (gap distribution geometric, no channel/mod-3 structure), and the RSSI
histogram of received packets extends down to −104 dBm, i.e. to the C3's Coded-S8 sensitivity: the missing events
are fades below sensitivity, not scanner scheduling. With adequate signal the firmware receives **98–100 %** of the
events, so nothing is lost in the host, the queue or Bluedroid. Controller options tested: AGC recorrect
(`CONFIG_BT_CTRL_AGC_RECORRECT_EN` + `CONFIG_BT_CTRL_CODED_AGC_RECORRECT_EN`) and adv-report flow control off
make no measurable difference at strong signal (at weak signal they could not be separated from the distance
changes); the coexistence Coded TX/RX time limit is already disabled by default.

Conclusion for "100 % Coded": scan settings cannot add margin. Use `phy: coded` (or both PHYs with a ≤ 20 ms 1M
window) and window = interval; then it is link budget: receiver placement/antenna, the thermometer's TX power
(pvvx `rf_tx_power`, up to +10 dBm), distance and walls.

## 9. test_lr_reset

Not executed: reverting requires either pulling the thermometer's battery (physical access) or writing `0xDD` over
a Coded-PHY GATT connection (not implemented – this build cannot open connections after the extended scan, see
limitations). The Long Range data should keep flowing into Home Assistant, so the thermometer stays in
LR mode and the flasher firmware is not restored. Procedure when wanted: pull the battery → thermometer advertises
legacy again → `esp32_ble_tracker`/flasher see it → `scripts/set_longrange.sh` re-enables LR from the flasher.

## Known limitations

* Reception of Coded-PHY advertisements is limited to roughly a quarter of the events with WiFi on (see §5) and
  about half without WiFi (§8d): the ESP32-C3 shares one radio between the 1M scan, the Coded scan and WiFi; the
  host side loses nothing (`dropped=0`, queue 23 deep, reports handled in < 1 ms).
* Extended scanning disables the legacy HCI scan/connect commands in the controller: `ble_client`,
  `bluetooth_proxy` connections (`active: true`) and the flasher's GATT client cannot coexist with this component.
* Reports longer than 62 B are decoded locally but not forwarded to `esp32_ble_tracker` / `bluetooth_proxy`
  (`BLEScanResult` / `BluetoothLERawAdvertisement` limit); pvvx frames are 29–30 B.
* Coded S=2 vs S=8 is not distinguished (Bluedroid without `CONFIG_BT_BLE_FEAT_ADV_CODING_SELECTION`).
* VERY_VERBOSE logging over the API costs heap and WiFi airtime; use DEBUG for production.
