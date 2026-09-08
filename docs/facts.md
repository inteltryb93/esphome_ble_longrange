# Fakty źródłowe (zebrane 2026-09-07)

## pvvx ATC_MiThermometer (`/home/mateusz/xiaomi_esp_flasher/reference/ATC_MiThermometer`)
* `src/app.h` `cfg_t.flg2`: bit5 `bt5phy` „support BT5.0 All PHY”, bit6 `longrange` „advertising in LongRange mode
  (сбрасывается после отключения питания = resetuje się po odłączeniu zasilania)”.
* `src/ble.c init_ble()`: `if (cfg.flg2.bt5phy) { blc_ll_init2MPhyCodedPhy_feature(); blc_ll_setDefaultConnCodingIndication(CODED_PHY_PREFER_S8);
  blc_ll_initChannelSelectionAlgorithm_2_feature(); if (cfg.flg2.longrange) adv_buf.ext_adv_init = EXT_ADV_Coded; }`
  (`src/ble.h`: `EXT_ADV_Off` = legacy, `EXT_ADV_Coded` = LE long range, `EXT_ADV_1M` = ext adv na 1M).
* `src/ble.c ev_adv_timeout()`: w trybie Coded → `blc_ll_setExtAdvParam(ADV_HANDLE0, ADV_EVT_PROP_EXTENDED_CONNECTABLE_UNDIRECTED,
  wrk.adv_interval, wrk.adv_interval + delay, BLT_ENABLE_ADV_ALL, OWN_ADDRESS_PUBLIC, …, BLE_PHY_CODED /*primary*/, 0, BLE_PHY_CODED /*secondary*/, ADV_SID_0, 0)`;
  nazwa przez `blc_ll_setExtScanRspData()` i `bls_ll_setScanRspData()`; dane reklamowe przez `blc_ll_setExtAdvData()` (`load_adv_data()`;
  w trybie Coded nazwa jest dopisywana do danych reklamowych: `|| adv_buf.ext_adv_init == EXT_ADV_Coded`).
* `src/ble.c set_adv_con_time()`: po rozłączeniu na ~1 s tryb `EXT_ADV_1M` (ext adv na 1M PHY), potem powrót do Coded.
* `src/cmd_parser.h`: `CMD_ID_LR_RESET = 0xDD` „Reset Long Range”; `CMD_ID_CFG_DEF = 0x56`.
* Zmiana bitów bt5phy/longrange przez `55` ustawia `CONNECTED_FLG_RESET_OF_DISCONNECT` → reboot po rozłączeniu (`cmd_parser.c`, `MASK_FLG2_REBOOT`).
* README pvvx: l. 34 „Supports Bluetooth v5.0+ LE Long Range (LE 1M/2M/Coded 500K/125K), CSA1/CSA2, Advertising Extensions: primary and secondary
  Coded PHY S=8, Connectable. LE Long Range - distance of 1 km”; l. 126 „ESPHome does not work with Bluetooth 5.0 and misses a lot of advertising
  packets”; l. 134 issue #297 (adapter USB BT5 w HA pod Linuxem); l. 138 „remove and insert the battery – the thermometer will switch to BT4.2”;
  l. 144 „To disable only the 'Long Range' option, use the code 0xDD”.
* Format danych reklamowych w LR: identyczny jak legacy (BTHome v2 `0xFCD2`, pvvx `0x181A` 15 B, atc1441 13 B, Mi `0xFE95`) – patrz
  `/home/mateusz/xiaomi_esp_flasher/components/xiaomi_esp_flasher/xiaomi_device.cpp: parse_advertisement()`.

## ESP-IDF 5.5.5, Bluedroid (`/home/mateusz/.cache/esphome/idf/frameworks/5.5.5/components/bt/host/bluedroid`)
* Kconfig: `BT_BLE_50_FEATURES_SUPPORTED` (`Kconfig.in:1391`) – wymagane dla API poniżej; w ESPHome ustawiasz przez
  `esp32: framework: sdkconfig_options:` albo `esp32.add_idf_sdkconfig_option()` w `__init__.py` komponentu.
* `api/include/api/esp_gap_ble_api.h`: `esp_ble_gap_set_ext_scan_params(const esp_ble_ext_scan_params_t*)` (l. 3880),
  `esp_ble_gap_start_ext_scan(uint32_t duration, uint16_t period)` (l. 3895), `esp_ble_gap_stop_ext_scan()`,
  `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` (l. 206) z `param->ext_adv_report.params` (`esp_ble_gap_ext_adv_report_t`: `event_type`, `addr_type`, `addr`,
  `primary_phy`, `secondry_phy`, `sid`, `tx_power`, `rssi`, `per_adv_interval`, `adv_data_len`, `adv_data`),
  `ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT`, `ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT`, `esp_ble_gap_set_preferred_default_phy()` (l. 3668),
  `esp_ble_gap_prefer_ext_connect_params_set()`.
* `api/include/api/esp_gattc_api.h`: `esp_ble_gattc_aux_open(gattc_if, remote_bda, remote_addr_type, is_direct)` (l. 403) – połączenie BLE 5 (Coded/2M).
* `esp_ble_ext_scan_params_t`: `own_addr_type`, `filter_policy`, `scan_duplicate`, `cfg_mask` (`ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK` | `ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK`),
  `uncoded_cfg` / `coded_cfg` (`esp_ble_ext_scan_cfg_t`: `scan_type`, `scan_interval`, `scan_window` w jednostkach 0,625 ms).
* ESP32-C3 obsługuje BLE 5.0: 2M PHY, Coded PHY (S=2/S=8), Advertising Extensions. Klasyczny ESP32 – nie (tylko BLE 4.2).

## ESPHome 2026.10.0-dev (`/home/mateusz/esphome/esphome/components`)
* `esp32_ble/ble.cpp`: obsługuje tylko `ESP_GAP_BLE_SCAN_RESULT_EVT` (l. 495, 650); brak `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` w całym drzewie
  (`grep -rn EXT_ADV_REPORT` = 0 trafień). Zdarzenia GAP/GATTC są kolejkowane z zadania BT i dostarczane w głównej pętli.
* `esp32_ble_tracker/esp32_ble_tracker.cpp start_scan_()`: `esp_ble_gap_set_scan_params()` + `esp_ble_gap_start_scanning(duration)` (legacy).
  Listenery: `ble_device_base::ESPBTDeviceListener::parse_device(const ESPBTDevice&)`; `ESPBTDevice::parse_scan_rst(const esp32_ble::BLEScanResult&)`.
* Rejestracja własnego handlera GAP z Pythona: `esp32_ble.register_gap_event_handler(parent, var)` (metoda C++ `gap_event_handler(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t*)`),
  oraz `register_gap_scan_event_handler` (tylko dla wyników legacy).
* `bluetooth_proxy`: przekazuje `BLEScanResult` do HA jako `BluetoothLERawAdvertisement` (sprawdź `bluetooth_proxy.cpp`, `api/api_pb2.*`).
* Szczegółowy przewodnik po API ESPHome użytych w projekcie-siostrze: `/home/mateusz/xiaomi_esp_flasher/docs/esphome_architecture.md`.

## Laboratorium
* ESP32-C3 na `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_64:E8:33:86:FB:00-if00`, WiFi 192.168.0.102, port USB „busy” po resecie.
* Adapter PC: Intel 8087:0a2b (BT 4.2, brak Coded PHY) – nie zobaczy termometru w LR.
* A4:C1:38:4A:E8:8C w trybie LR od 2026-09-07 22:09 (cfg flg2 0x70 wysłane z zakładki Configure flashera). Pozostałe termometry legacy.
* Flasher (projekt-siostra) na ESP32: API `POST /api/device/<mac>/connect`, `/cmd {"hex":"..."}`, `/disconnect` – działa tylko dla urządzeń legacy.
