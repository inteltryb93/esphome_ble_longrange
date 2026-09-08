# Architecture of the `ble_longrange_scanner` component

## What is reused from ESPHome

| Need | ESPHome | Notes |
|---|---|---|
| Controller + Bluedroid, stack life cycle | `esp32_ble` (`ESP32BLE`, `Parented`) | the component waits in `loop()` until `parent->is_active()`; on stack `disable()`/`enable()` it re-hooks (after a re-init Bluedroid has ESPHome's callback again). |
| BT-task → main-loop event queue | `esphome/core/event_pool.h` + `lock_free_queue.h` | same pattern as `ESP32BLE::ble_events_`; own pool of 24 slots × ~270 B (an extended report carries up to 251 B of data, ESPHome's `BLEEvent` cannot hold it). |
| HA entities | `sensor`, `text_sensor`, `binary_sensor` | created in Python from the `devices:` list; default names `<name> Temperature` etc. |
| MAC formatting / logging | `format_mac_addr_upper`, `ESP_LOGx` | tags `ble_lr` (`ble_lr.scan`, `ble_lr.dev`). |
| Compatibility with the rest of the BLE ecosystem (`bluetooth_proxy`, sensor platforms, triggers) | `esp32_ble_tracker::gap_scan_event_handler(const BLEScanResult&)` (public) | **forwarding**: every extended report (legacy 1M and Coded) up to 62 B is converted into a `BLEScanResult` and handed to the tracker, which then serves listeners and the proxy itself. **Tracker scan emulation**: its `esp_ble_gap_set_scan_params/start_scanning/stop_scanning` calls are redirected by the linker (`-Wl,--wrap`) to the component, which delivers synthetic `SCAN_PARAM_SET/START/STOP_COMPLETE` (status 0) through the public `gap_event_handler()` from the main loop, plus `ESP_GAP_SEARCH_INQ_CMPL_EVT` after `duration` – the tracker cycles IDLE→STARTING→RUNNING→IDLE normally, `bluetooth_proxy` reports RUNNING and the mode to HA, and the tracker's `active/passive` (YAML or HA) drives the 1M part of the extended scan. Measured: the C3 controller rejects (status 12, Command Disallowed) an extended scan while a legacy one runs and vice versa. |
| sdkconfig | `esp32.add_idf_sdkconfig_option` | `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_EXTEND_SCAN_EN=y`; BLE 4.2 stays `y` (ESPHome default) so the tracker/client still compile. |

## What was written from scratch and why

1. **GAP callback hook** (`ble_longrange_scanner.cpp: gap_callback_`): ESPHome queues neither
   `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` nor `*_EXT_SCAN_*_COMPLETE_EVT` (see `docs/research.md` §1.1) and its
   `gap_event_handler` is protected. The component does `prev = esp_ble_gap_get_callback();
   esp_ble_gap_register_callback(own)`; the own callback (BTC context) handles the 5 extended-scan events (copy into
   the pool + push into the queue) and passes everything else to `prev` – ESPHome sees no difference. Zero changes
   in the ESPHome checkout.
2. **Extended-scan state machine**: `IDLE → HOOKED → PARAMS_SET(wait) → STARTING(wait) → RUNNING`, errors → `FAILED`
   with a retry after 5 s (backoff up to 60 s); `duration=0, period=0` = continuous scan (no periodic restart as in
   the tracker). Watchdog: no reports for `report_timeout` (default 120 s) → `stop_ext_scan` → restart.
3. **Parameters**: `cfg_mask = 1M | Coded` (or one PHY via `phy:`); 1M: active/passive per YAML / tracker, Coded:
   passive (pvvx in LR is not scannable, the name is inside the AUX_ADV_IND); interval/window per PHY
   (default 1M 400/80 ms, Coded 400/300 ms); `scan_duplicate = DISABLE`, `filter_policy = ALLOW_ALL`,
   `own_addr_type = PUBLIC`.
4. **Fragment reassembly** (`data_status = incomplete`) per address+SID (2 slots × 251 B) and **legacy ADV +
   SCAN_RSP merging** (a scannable adv is held ≤ 300 ms, like `scan_response_merger.h` does for rp2/bk72xx, which
   esp32 does not compile) – so the tracker/proxy get one frame just like from a native 4.2 scan.
5. **Advertisement parser** (`adv_parser.*`, no allocations): AD structures → flags/name/service data; formats
   BTHome v2 (0xFCD2), pvvx custom (0x181A/15 B), atc1441 (0x181A/13 B), Mi (0xFE95) – ported from
   `xiaomi_esp_flasher/xiaomi_device.cpp: parse_advertisement()`.
6. **Devices and entities**: table `Device{mac, name, sensors…, stats}`; for every report with a matching MAC:
   publish temperature/humidity/battery/voltage/RSSI/packet counter, `text_sensor` PHY ("1M", "2M", "Coded"),
   `binary_sensor` Long Range (primary PHY = Coded), `text_sensor` format. Per-device counters (legacy/coded) for
   the reception statistics.
7. **Tracker legacy-scan emulation** (`__wrap_esp_ble_gap_*` in `ble_longrange_scanner.cpp`, linker flags in
   `__init__.py`): the only callers of these APIs in an ESPHome build are in `esp32_ble_tracker`; `__real_*`
   remain reachable (used when the component is not initialised yet). This keeps the YAML identical to a plain
   Bluetooth proxy (no `continuous: false`).
8. **Runtime tuning**: `set_scan_ms()` / `set_coex_prefer_bt()` / `restart_scan()` exposed in YAML as
   `api: actions:` (`set_scan`, `set_coex`, `restart_scan`) – `scripts/sweep.py` switches parameters over the
   native API and reads the `reports_1m` / `reports_coded` sensors, without re-flashing and without streaming logs
   (WiFi traffic disturbs the measurement through coexistence). The `coex_prefer_bt` option sets
   `esp_coex_preference_set(ESP_COEX_PREFER_BT)` (the mechanism `esp32_ble_tracker` uses during GATT connections).
9. **Statistics and diagnostics** (`ble_lr` INFO every `stats_interval`, default 60 s): reports/min per PHY, queue
   drops, scan restarts, free / minimum heap, `esp_reset_reason()`. Optional global entities: `reports_1m`,
   `reports_coded`, `free_heap`, `scanner_state`.

## Data flow

```
C3 controller ─HCI LE Ext Adv Report─▶ Bluedroid (btu → btm → btc) ─▶ gap_callback_ [BTC task]
   ├─ ext scan events  → EventPool/LockFreeQueue ─▶ loop() [main]
   └─ everything else  → ESP32BLE::gap_event_handler (original) → ESPHome queue
loop(): pop → (fragments) → (scan rsp merge) → handle_report_()
   ├─ Device match → adv_parser → sensor::publish_state … (HA through api)
   └─ tracker_ → gap_scan_event_handler(BLEScanResult) → listeners / bluetooth_proxy → HA raw adv
```

## Measured limitations (docs/test_report.md §5, §8d, §8e)
* Coded-PHY reception: 5–8 of 24 events/min (22–35 %) with WiFi on, 50–70 % without WiFi at −85…−95 dBm,
  98–100 % with a strong signal – the losses are RF fades below the Coded-S8 sensitivity and radio time taken by
  WiFi coexistence; the host loses nothing (`dropped=0`). `coex_prefer_bt` gives +60 % at identical windows with
  WiFi on; the Coded window should equal the interval; a 20 ms 1M window costs nothing.

## Known limitations
* Reports longer than 62 B are not forwarded to the tracker/proxy (`BLEScanResult` / `BluetoothLERawAdvertisement`
  limit); the component's own entities work for the full 251 B.
* After the extended scan is in use the controller refuses legacy commands: `ble_client` / `bluetooth_proxy` with
  active connections (`esp_ble_gattc_open` = legacy create connection) are not supported in this build;
  `bluetooth_proxy` only with `active: false` (advertisements). A connection in LR would need
  `esp_ble_gattc_aux_open()`.
* Coded S2 vs S8 is not distinguished without `CONFIG_BT_BLE_FEAT_ADV_CODING_SELECTION` (pvvx transmits S8).
