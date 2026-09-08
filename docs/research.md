# Research (2026-09-07) – extended scan / Coded PHY in ESPHome on ESP32-C3

File references: `E` = `/home/mateusz/esphome/esphome/components`, `IDF` =
`/home/mateusz/.cache/esphome/idf/frameworks/5.5.5/components/bt`, `PVVX` =
`/home/mateusz/xiaomi_esp_flasher/reference/ATC_MiThermometer/src`.

## 1. ESPHome 2026.10.0-dev

### 1.1 `esp32_ble` – stack initialisation and the GAP callback
* `E/esp32_ble/ble.cpp:72` `ESP32BLE::setup()` only sets the `ENABLE` state; the real `ble_setup_()` (controller +
  Bluedroid) runs **in `loop()`** on the first iteration (`loop_handle_state_transition_not_active_()`,
  l. 584–599). Consequence: other components cannot touch the stack in their `setup()`, they must wait until
  `parent->is_active()` (`ble.h:107`).
* `ble.cpp:285` `esp_ble_gap_register_callback(ESP32BLE::gap_event_handler)` – one GAP callback for the whole
  Bluedroid. `gap_event_handler` is **`protected static`** (`ble.h:171`), so an external component cannot call it.
  Bluedroid however offers `esp_ble_gap_get_callback()` (`IDF/host/bluedroid/api/esp_gap_ble_api.c:28`, header
  l. 2932) and `esp_ble_gap_register_callback()` is just `btc_profile_cb_set()` (l. 21–26) – one can **take the
  previous callback and register an own one that chains unhandled events**. No patch needed.
* `ble.cpp:646–700` `gap_event_handler` (BTC task context!) queues only: `SCAN_RESULT`, legacy scan/adv completes,
  `READ_RSSI`, security events, `UPDATE_CONN_PARAMS`; `PHY_UPDATE_COMPLETE` and `CHANNEL_SELECT_ALGORITHM` are ignored;
  **everything else – including `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` and `*_EXT_SCAN_*_COMPLETE_EVT` – falls into
  `default` and is logged as "Ignoring unexpected GAP event type"** (l. 699). `grep -rn EXT_ADV_REPORT E/` = 0.
* Queue: `BLEEvent` (`ble_event.h:198–217`) holds only a `BLEScanResult` (73 B) or a status – no room for an
  extended report (up to 251 B of data). GAP events are delivered in `ESP32BLE::loop()` (`ble.cpp:454–567`) through
  `gap_scan_event_callbacks_` (legacy result) and `gap_event_callbacks_` (`register_gap_event_handler` in
  `__init__.py:177`, only for events from the `GAP_SCAN_COMPLETE_EVENTS`/`GAP_ADV_COMPLETE_EVENTS`/security lists).
* Threading model: Bluedroid callbacks run in the BTC task; ESPHome copies the event into a pool (`EventPool` +
  `LockFreeQueue`, `core/event_pool.h`, `core/lock_free_queue.h`) and processes it in the main loop. The component
  follows the same pattern.

### 1.2 `esp32_ble_tracker` – legacy scan
* `E/esp32_ble_tracker/esp32_ble_tracker.cpp:243–294` `start_scan_()`: `esp_ble_gap_set_scan_params()` +
  `esp_ble_gap_start_scanning(duration)` – BLE 4.2 API (`BLE_42_SCAN_EN`). Starts only when
  `scanner_state_ == IDLE && scan_continuous_` (`loop()`, l. 181–194); with `continuous: false` the tracker **never
  starts a scan**, but everything else (listeners, `bluetooth_proxy`) works.
* Result: `gap_scan_event_handler(const BLEScanResult&)` (l. 350) → `process_scan_result_()` (l. 464–511):
  `raw_advertisement_callback_` (bluetooth_proxy) + `ESPBTDevice::parse_scan_rst()` → `listeners_`,
  `neutral_listeners_`, `clients_`. `gap_scan_event_handler` is **public** (`esp32_ble_tracker.h:222`) – an own
  `BLEScanResult` can be handed to it (`esp32_ble/ble_scan_result.h`: `bda` MSB-first, `ble_adv[62]`,
  `adv_data_len`, `scan_rsp_len`, `search_evt = ESP_GAP_SEARCH_INQ_RES_EVT`). Limit: 62 B of data.
  `gap_event_handler(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*)` (l. 221) is public as well – used for the
  synthetic scan completion events of the legacy-scan emulation.
* `ESPBTDevice::parse_scan_rst()` (`E/ble_device_base/ble_device.cpp:135`) reverses `bda` to LSB-first and calls
  `from_scan_result(mac, rssi, addr_type, data, len)` (l. 344).
* Public tracker API useful for coexistence: `set_scan_continuous(bool)`, `stop_scan()`, `get_scanner_state()`,
  `get_scan_active()` (`esp32_ble_tracker.h:176–228`).
* `BLEHub` (`E/ble_device_base/ble_hub.h`, `ble_hub_impl.h`) is a compile-time alias for **one of the in-tree
  trackers** ("out-of-tree BLE hubs are not supported", `ble_device_base/__init__.py`), so `bluetooth_proxy` and the
  sensor platforms (e.g. `xiaomi_ble`) can only be fed through `esp32_ble_tracker` – hence the forwarding.
* Only the tracker calls `esp_ble_gap_set_scan_params/start_scanning/stop_scanning` in an ESPHome build
  (`grep -rn` over `E/`), and ESPHome itself already links with `-Wl,--wrap=esp_panic_handler`
  (`E/esp32/__init__.py:2438`) – wrapping the three scan calls at link time is a supported mechanism.

### 1.3 `bluetooth_proxy`
* `E/bluetooth_proxy/bluetooth_proxy.cpp:81` subscribes `hub_->set_raw_advertisement_callback()`; l. 93–115
  `on_raw_advertisement_()` copies `address` (u64), `rssi`, `address_type`, `data` (max 62 B, `static_assert`
  l. 23) into `api::BluetoothLERawAdvertisement` and sends batches. HA gets no PHY information, only the raw AD –
  the BTHome integration decodes it regardless of the PHY. `bluetooth_scanner_set_mode()` (l. 561–571) calls
  `set_scan_active`, `stop_scan` and `set_scan_continuous(true)` on the tracker; the scanner state is pushed to HA
  through `set_scanner_state_callback` (`USE_BLE_SCANNER_STATE_CALLBACK`), and HA (bleak-esphome) uses it only for
  `current_mode` – its "scanning" flag comes from the advertisement flow.
* One advertisement subscriber slot, "newest subscriber wins" (`subscribe_api_connection`, l. 728–745).

### 1.4 sdkconfig
* Options from YAML `esp32.framework.sdkconfig_options` and from `esp32.add_idf_sdkconfig_option()` land in
  `CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]` (`E/esp32/__init__.py:689`). ESPHome's defaults are set with
  `set_idf_sdkconfig_default()` (l. 677, "unless already set") in `_reconcile_network_sdkconfig()` (FINAL priority,
  l. 2316–2323): **`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n`** – ESPHome
  explicitly disables BLE 5.0. A value from a component/YAML wins (it is set earlier).
* Confirmed on the running flasher build: `xiaomi_esp_flasher/yaml/.esphome/build/xiaomi-esp-flasher/
  sdkconfig.xiaomi-esp-flasher`: `# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set`, `CONFIG_BT_BLE_42_*=y`,
  `CONFIG_BT_CTRL_BLE_SCAN_DUPL=y`, `CONFIG_BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM=100`, `BT_ACL_CONNECTIONS=2`.

## 2. ESP-IDF 5.5.5 / Bluedroid BLE 5.0

* Kconfig (`IDF/host/bluedroid/Kconfig.in:1391`): `BT_BLE_50_FEATURES_SUPPORTED` "default y" (except ESP32),
  `BT_BLE_50_EXTEND_SCAN_EN` (l. 1414, default y), `BT_BLE_42_FEATURES_SUPPORTED` (l. 1608, default n on C3),
  `BT_BLE_42_SCAN_EN` (l. 1632). The help says "4.2 and 5.0 cannot be used simultaneously", but this is **not
  enforced**: `bt_target.h:243–256` yields `BLE_50_FEATURE_SUPPORT=TRUE` and `BLE_42_FEATURE_SUPPORT=TRUE` when both
  options are `y`; both API sets are then compiled (`esp_gap_ble_api.c:61–110` legacy scan under
  `BLE_42_SCAN_EN`, l. 1709–1763 ext scan under `BLE_50_EXTEND_SCAN_EN`). The restriction is in the controller:
  a legacy scan running while an extended one is requested (or vice versa) is refused with status 12 (Command
  Disallowed) – measured, see `docs/test_report.md` §8b.
* API (`IDF/host/bluedroid/api/include/api/esp_gap_ble_api.h`): `esp_ble_gap_set_ext_scan_params()` l. 3880,
  `esp_ble_gap_start_ext_scan(duration, period)` l. 3895 (duration 0 = until stopped, period 0 = no cycle),
  `esp_ble_gap_stop_ext_scan()` l. 3904. Structures: `esp_ble_ext_scan_params_t` l. 1026–1033
  (`own_addr_type`, `filter_policy`, `scan_duplicate`, `cfg_mask` = `ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK` 0x01 |
  `..._CODE_MASK` 0x02, `uncoded_cfg`/`coded_cfg` = `{scan_type, scan_interval, scan_window}` in 0.625 ms).
* Events: `ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT` (l. 201, `param->set_ext_scan_params.status`),
  `ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT` (202, `ext_scan_start.status`), `..._STOP_COMPLETE_EVT` (203),
  `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` (206, `param->ext_adv_report.params`), `ESP_GAP_BLE_SCAN_TIMEOUT_EVT`.
* `esp_ble_gap_ext_adv_report_t` (l. 1118–1155): `event_type` (bits: 0x01 connectable, 0x02 scannable, 0x04
  directed, 0x08 scan response, **0x10 legacy PDU**; legacy types `ESP_BLE_LEGACY_ADV_TYPE_IND` 0x13,
  `SCAN_IND` 0x12, `NONCON_IND` 0x10, `SCAN_RSP_TO_ADV_IND` 0x1b, `SCAN_RSP_TO_ADV_SCAN_IND` 0x1a, l. 943–948),
  `addr_type`, `addr`, `primary_phy` / `secondly_phy` (with `CONFIG_BT_BLE_FEAT_PAWR_EN` the names are
  `primary_phy`/`secondary_phy` – handled by a macro in the code), values `ESP_BLE_GAP_PHY_1M`=1, `2M`=2, `CODED`=3
  (l. 892–894; without `ADV_CODING_SELECTION` Coded S2/S8 are not distinguished), `sid`, `tx_power`, `rssi`,
  `per_adv_interval`, `dir_addr_type`, `dir_addr`, `data_status` (0 complete / 1 incomplete-more / 2 truncated,
  l. 927–929), `adv_data_len`, `adv_data[251]`.
* Report path: HCI `LE Extended Advertising Report` → `btu_hcif.c:2535` `btu_ble_ext_adv_report_evt()` (parses
  `evt_type & 0x1F`, `data_status`, PHY, RSSI, every report separately – **no merging of adv + scan response**,
  unlike the 4.2 path) → `btm_ble_5_gap.c:1276` → `btc_gap_ble.c:1232` (copies into
  `param.ext_adv_report.params`, zeroes the 251 B buffer) → application callback. Fragments (`data_status=1`)
  arrive as further reports from the same address/SID – reassembly is up to the application.
* An extended scan with mask 1M+Coded also receives legacy ADV (with `event_type` 0x1x, `primary_phy=1M`), so one
  scan replaces the tracker's legacy scan. Scan responses of legacy devices come as separate reports (0x1b/0x1a) –
  the component merges them with the preceding ADV_IND (max 300 ms) like `ble_device_base/scan_response_merger.h`
  does for rp2/bk72xx (that code is not compiled on esp32 – `USE_BLE_SCAN_RESPONSE_MERGER`, hence an own
  mini-implementation).
* Connections in LR (not implemented): `esp_ble_gap_prefer_ext_connect_params_set()` (l. 4007),
  `esp_ble_gattc_aux_open()` (`esp_gattc_api.h:403`), `esp_ble_gap_set_preferred_default_phy()` (l. 3668).
* ESP32-C3 controller: `SOC_BLE_50_SUPPORTED`, Coded PHY S2/S8, ext adv/scan; `BT_CTRL_BLE_SCAN_DUPL`
  (the duplicate filter only acts when `scan_duplicate=ENABLE` in the parameters – we set DISABLE),
  `BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM=100` (the controller may discard reports when the host lags),
  `BT_CTRL_COEX_PHY_CODED_TX_RX_TLIM` (Coded TX/RX time limit under coexistence, disabled by default),
  `BT_CTRL_AGC_RECORRECT_EN` + `BT_CTRL_CODED_AGC_RECORRECT_EN` (HW AGC recorrect for Coded PHY, default n; no
  measurable effect in the tests).

## 3. pvvx ATC_MiThermometer (fw 5.9)

* `PVVX/app.h:115–116`: `cfg.flg2.bt5phy` (bit5, 0x20), `cfg.flg2.longrange` (bit6, 0x40, "сбрасывается после
  отключения питания" – cleared after a power loss).
* `PVVX/ble.c:649–672` `init_ble()`: `adv_buf.ext_adv_init = EXT_ADV_Off`; with `bt5phy`:
  `blc_ll_init2MPhyCodedPhy_feature()`, `blc_ll_setDefaultConnCodingIndication(CODED_PHY_PREFER_S8)`, CSA#2; with
  `longrange` in addition: `blc_ll_setDefaultExtAdvCodingIndication(ADV_HANDLE0, CODED_PHY_PREFER_S8)`,
  `ll_module_adv_cb = _blt_ext_adv_proc`, `adv_buf.ext_adv_init = EXT_ADV_Coded`,
  `blc_ll_setDefaultPhy(PHY_TRX_PREFER, BLE_PHY_CODED, BLE_PHY_CODED)`.
* `PVVX/ble.c:323–370` `ev_adv_timeout()`: in Coded mode `blc_ll_setExtAdvParam(ADV_HANDLE0,
  ADV_EVT_PROP_EXTENDED_CONNECTABLE_UNDIRECTED, wrk.adv_interval, …, BLT_ENABLE_ADV_ALL, OWN_ADDRESS_PUBLIC, …,
  BLE_PHY_CODED /*primary*/, 0, BLE_PHY_CODED /*secondary*/, ADV_SID_0, 0)`; name through
  `blc_ll_setExtScanRspData()` (l. 362) and `bls_ll_setScanRspData()`; `blc_ll_setExtAdvEnable_1()` (l. 369).
  Expected report: `event_type = 0x01` (connectable, non-scannable, non-legacy), `primary_phy = secondary_phy =
  Coded (3)`, `sid = 0`, `addr_type = public`, `data_status = complete`.
* `PVVX/ble.c:190–256` `app_advertise_prepare_handler()`: called before every advertising event (every
  `adv_interval` = 2.5 s); the BTHome frames alternate (`call_count & 1`: temperature/humidity/battery% and
  voltage+binary objects), the packet id (`send_count`) increments once per measurement
  (`measure_interval` × `adv_interval` = 10 s).
* `PVVX/ble.c:600–621` `load_adv_data()`: in `EXT_ADV_Coded` mode **the name (0x09) is appended to the advertising
  data** (`|| adv_buf.ext_adv_init == EXT_ADV_Coded`), flags (0x01) when `cfg.flg2.adv_flags` (here flg2=0x70 →
  adv_flags=1), and everything goes through `blc_ll_setExtAdvData(…, DATA_OPER_COMPLETE, …)`. A passive scan on
  Coded is enough (the packet is not scannable anyway).
* `PVVX/ble.c:387–405` `set_adv_con_time()`: after a disconnect ~1 s of `EXT_ADV_1M` (ext adv on 1M, legacy PDU
  `ADV_EVT_PROP_LEGACY_CONNECTABLE_SCANNABLE_UNDIRECTED`, l. 329–345) and back to Coded (`restore`).
* LR reset: `PVVX/cmd_parser.h:62` `CMD_ID_LR_RESET = 0xDD`, `:45` `CMD_ID_CFG_DEF = 0x56` (handled in
  `cmd_parser.c:880`, `:382`); README l. 138 (battery pull → BT4.2), l. 144 (0xDD only LR).
* Data format in LR = as in legacy: BTHome v2 service data 0xFCD2 (`bthome_beacon.c`: `bthome_data_beacon()`
  l. 222: info byte, 0x00 pid, 0x01 batt%, 0x02 temp(i16 ×0.01), 0x03 humi(u16 ×0.01), 0x0C vbat(mV) –
  per the sister project's parser `xiaomi_esp_flasher/xiaomi_device.cpp:73–184`), pvvx 0x181A (15 B), atc1441
  (13 B), Mi 0xFE95. Parser ported to `components/ble_longrange_scanner/adv_parser.*`.

## 4. Lab (state confirmed at 22:18)
* `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_64:E8:33:86:FB:00-if00` → `/dev/ttyACM1`
  (root:dialout, no `fuser`), the flasher alive on 192.168.0.102 (`/api/status`: uptime 884 s, heap 56 KB,
  `ble_state: scanning`), `A4:C1:38:4A:E8:8C` "Living Room" **offline since 22:05:11** (last legacy adv), last
  legacy values: 23.25 °C / 63.08 % / 2911 mV; `last_error: BLE_TIMEOUT` (failed legacy connect to the LR device).
* PC adapter `hci0` Intel 8087:0a2b HCI 4.2 – no extended scan.
* Other thermometers (legacy, BTHome v2): 0B:5F:4E, B0:E4:4A, AA:01:81, B8:31:E8, 93:72:DB, 3D:A8:7F.
