# Research (2026-09-07) – ext scan / Coded PHY w ESPHome na ESP32-C3

Odwołania do plików: `E` = `/home/mateusz/esphome/esphome/components`, `IDF` =
`/home/mateusz/.cache/esphome/idf/frameworks/5.5.5/components/bt`, `PVVX` =
`/home/mateusz/xiaomi_esp_flasher/reference/ATC_MiThermometer/src`.

## 1. ESPHome 2026.10.0-dev

### 1.1 `esp32_ble` – inicjalizacja stosu i callback GAP
* `E/esp32_ble/ble.cpp:72` `ESP32BLE::setup()` tylko ustawia stan `ENABLE`; właściwy `ble_setup_()` (kontroler +
  Bluedroid) wykonuje się **w `loop()`** przy pierwszej iteracji (`loop_handle_state_transition_not_active_()`,
  l. 584–599). Wniosek: inne komponenty nie mogą nic robić ze stosem w swoim `setup()`, muszą czekać aż
  `parent->is_active()` (`ble.h:107`).
* `ble.cpp:285` `esp_ble_gap_register_callback(ESP32BLE::gap_event_handler)` – jeden callback GAP dla całego
  Bluedroid. `gap_event_handler` jest **`protected static`** (`ble.h:171`), więc komponent zewnętrzny nie może go
  wywołać. Bluedroid ma jednak `esp_ble_gap_get_callback()` (`IDF/host/bluedroid/api/esp_gap_ble_api.c:28`,
  nagłówek l. 2932) i `esp_ble_gap_register_callback()` to tylko `btc_profile_cb_set()` (l. 21–26) – można
  **pobrać poprzedni callback i zarejestrować własny, który łańcuchuje nieobsłużone zdarzenia**. Bez patcha.
* `ble.cpp:646–700` `gap_event_handler` (kontekst zadania BTC!) kolejkuje tylko: `SCAN_RESULT`, komplety legacy
  scan/adv, `READ_RSSI`, zdarzenia security, `UPDATE_CONN_PARAMS`; `PHY_UPDATE_COMPLETE` i `CHANNEL_SELECT_ALGORITHM`
  ignoruje; **wszystko inne – w tym `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` i `*_EXT_SCAN_*_COMPLETE_EVT` – trafia do
  `default` i jest logowane jako „Ignoring unexpected GAP event type”** (l. 699). `grep -rn EXT_ADV_REPORT E/` = 0.
* Kolejka: `BLEEvent` (`ble_event.h:198–217`) przechowuje tylko `BLEScanResult` (73 B) albo status – nie ma miejsca
  na raport ext (do 251 B danych). Zdarzenia GAP są dostarczane w `ESP32BLE::loop()` (`ble.cpp:454–567`) przez
  `gap_scan_event_callbacks_` (wynik legacy) i `gap_event_callbacks_` (`register_gap_event_handler` w
  `__init__.py:177`, tylko dla zdarzeń z listy `GAP_SCAN_COMPLETE_EVENTS`/`GAP_ADV_COMPLETE_EVENTS`/security).
* Model wątków: callbacki Bluedroid działają w zadaniu BTC; ESPHome kopiuje zdarzenie do puli (`EventPool` +
  `LockFreeQueue`, `core/event_pool.h`, `core/lock_free_queue.h`) i przetwarza w pętli głównej. Ten sam wzorzec
  stosuje własny komponent.

### 1.2 `esp32_ble_tracker` – skan legacy
* `E/esp32_ble_tracker/esp32_ble_tracker.cpp:243–294` `start_scan_()`: `esp_ble_gap_set_scan_params()` +
  `esp_ble_gap_start_scanning(duration)` – API BLE 4.2 (`BLE_42_SCAN_EN`). Start tylko gdy
  `scanner_state_ == IDLE && scan_continuous_` (`loop()`, l. 181–194); z `continuous: false` tracker **nigdy nie
  startuje skanu**, ale cała reszta (listenery, `bluetooth_proxy`) działa.
* Wynik: `gap_scan_event_handler(const BLEScanResult&)` (l. 350) → `process_scan_result_()` (l. 464–511):
  `raw_advertisement_callback_` (bluetooth_proxy) + `ESPBTDevice::parse_scan_rst()` → `listeners_`,
  `neutral_listeners_`, `clients_`. `gap_scan_event_handler` jest **publiczne** (`esp32_ble_tracker.h:222`) –
  można mu podać własny `BLEScanResult` (`esp32_ble/ble_scan_result.h`: `bda` MSB-first, `ble_adv[62]`,
  `adv_data_len`, `scan_rsp_len`, `search_evt = ESP_GAP_SEARCH_INQ_RES_EVT`). Ograniczenie: 62 B danych.
* `ESPBTDevice::parse_scan_rst()` (`E/ble_device_base/ble_device.cpp:135`) odwraca `bda` do LSB-first i woła
  `from_scan_result(mac, rssi, addr_type, data, len)` (l. 344).
* Publiczne API trackera przydatne do koegzystencji: `set_scan_continuous(bool)`, `stop_scan()`,
  `get_scanner_state()` (`esp32_ble_tracker.h:177, 218, 228`).
* `BLEHub` (`E/ble_device_base/ble_hub.h`, `ble_hub_impl.h`) to alias kompilacyjny na **jeden z trackerów
  in-tree** („out-of-tree BLE hubs are not supported”, `ble_device_base/__init__.py`), więc `bluetooth_proxy`
  i platformy sensorów (np. `xiaomi_ble`) da się zasilić tylko przez `esp32_ble_tracker` – stąd forwarding.

### 1.3 `bluetooth_proxy`
* `E/bluetooth_proxy/bluetooth_proxy.cpp:81` subskrybuje `hub_->set_raw_advertisement_callback()`; l. 93–115
  `on_raw_advertisement_()` kopiuje `address` (u64), `rssi`, `address_type`, `data` (max 62 B, `static_assert`
  l. 23) do `api::BluetoothLERawAdvertisement` i wysyła partiami. HA nie dostaje informacji o PHY, tylko surowe
  AD – integracja BTHome dekoduje je niezależnie od PHY. `bluetooth_scanner_set_mode()` (l. 561–571) może
  ustawić `scan_continuous(true)` i wystartować skan legacy – trzeba to blokować w naszym komponencie.

### 1.4 sdkconfig
* Opcje z YAML `esp32.framework.sdkconfig_options` i z `esp32.add_idf_sdkconfig_option()` lądują w
  `CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]` (`E/esp32/__init__.py:689`). Domyślne wartości ESPHome ustawia
  `set_idf_sdkconfig_default()` (l. 677, „unless already set”) w `_reconcile_network_sdkconfig()` (priorytet
  FINAL, l. 2316–2323): **`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n`** –
  czyli ESPHome jawnie wyłącza BLE 5.0. Wartość z komponentu/YAML wygrywa (jest ustawiona wcześniej).
* Potwierdzenie na działającym buildzie flashera: `xiaomi_esp_flasher/yaml/.esphome/build/xiaomi-esp-flasher/
  sdkconfig.xiaomi-esp-flasher`: `# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set`, `CONFIG_BT_BLE_42_*=y`,
  `CONFIG_BT_CTRL_BLE_SCAN_DUPL=y`, `CONFIG_BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM=100`, `BT_ACL_CONNECTIONS=2`.

## 2. ESP-IDF 5.5.5 / Bluedroid BLE 5.0

* Kconfig (`IDF/host/bluedroid/Kconfig.in:1391`): `BT_BLE_50_FEATURES_SUPPORTED` „default y” (poza ESP32),
  `BT_BLE_50_EXTEND_SCAN_EN` (l. 1414, default y), `BT_BLE_42_FEATURES_SUPPORTED` (l. 1608, default n na C3),
  `BT_BLE_42_SCAN_EN` (l. 1632). Opis mówi „4.2 i 5.0 nie mogą być używane jednocześnie”, ale to **nie jest
  wymuszone**: `bt_target.h:243–256` daje `BLE_50_FEATURE_SUPPORT=TRUE` i `BLE_42_FEATURE_SUPPORT=TRUE` gdy obie
  opcje są `y`; oba zestawy API są wtedy kompilowane (`esp_gap_ble_api.c:61–110` legacy scan pod
  `BLE_42_SCAN_EN`, l. 1709–1763 ext scan pod `BLE_50_EXTEND_SCAN_EN`). Ograniczenie jest po stronie
  kontrolera: po użyciu komend HCI „extended” kontroler odrzuca komendy legacy (BT Core 5.x, Vol 4 E 7.8.9/7.8.64).
  Dlatego z ext scanem **tracker nie może startować skanu legacy** – ale może być skompilowany.
* API (`IDF/host/bluedroid/api/include/api/esp_gap_ble_api.h`): `esp_ble_gap_set_ext_scan_params()` l. 3880,
  `esp_ble_gap_start_ext_scan(duration, period)` l. 3895 (duration 0 = do odwołania, period 0 = bez cyklu),
  `esp_ble_gap_stop_ext_scan()` l. 3904. Struktury: `esp_ble_ext_scan_params_t` l. 1026–1033
  (`own_addr_type`, `filter_policy`, `scan_duplicate`, `cfg_mask` = `ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK` 0x01 |
  `..._CODE_MASK` 0x02, `uncoded_cfg`/`coded_cfg` = `{scan_type, scan_interval, scan_window}` w 0,625 ms).
* Zdarzenia: `ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT` (l. 201, `param->set_ext_scan_params.status`),
  `ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT` (202, `ext_scan_start.status`), `..._STOP_COMPLETE_EVT` (203),
  `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` (206, `param->ext_adv_report.params`), `ESP_GAP_BLE_SCAN_TIMEOUT_EVT`.
* `esp_ble_gap_ext_adv_report_t` (l. 1118–1155): `event_type` (bity: 0x01 connectable, 0x02 scannable, 0x04
  directed, 0x08 scan response, **0x10 legacy PDU**; legacy typy `ESP_BLE_LEGACY_ADV_TYPE_IND` 0x13,
  `SCAN_IND` 0x12, `NONCON_IND` 0x10, `SCAN_RSP_TO_ADV_IND` 0x1b, `SCAN_RSP_TO_ADV_SCAN_IND` 0x1a, l. 943–948),
  `addr_type`, `addr`, `primary_phy` / `secondry_phy` (gdy `CONFIG_BT_BLE_FEAT_PAWR_EN` nazwy to
  `primary_phy`/`secondary_phy` – w kodzie makro), wartości `ESP_BLE_GAP_PHY_1M`=1, `2M`=2, `CODED`=3 (l. 892–894;
  bez `ADV_CODING_SELECTION` Coded S2/S8 nie jest rozróżniane), `sid`, `tx_power`, `rssi`, `per_adv_interval`,
  `dir_addr_type`, `dir_addr`, `data_status` (0 complete / 1 incomplete-more / 2 truncated, l. 927–929),
  `adv_data_len`, `adv_data[251]`.
* Ścieżka raportu: HCI `LE Extended Advertising Report` → `btu_hcif.c:2535` `btu_ble_ext_adv_report_evt()`
  (parsuje `evt_type & 0x1F`, `data_status`, PHY, RSSI, każdy raport osobno – **brak scalania adv + scan
  response**, w przeciwieństwie do ścieżki 4.2) → `btm_ble_5_gap.c:1276` → `btc_gap_ble.c:1232` (kopiuje do
  `param.ext_adv_report.params`, zeruje bufor 251 B) → callback aplikacji. Fragmenty (`data_status=1`)
  przychodzą jako kolejne raporty z tego samego adresu/SID – scalanie jest po stronie aplikacji.
* Ext scan z maską 1M+Coded odbiera także legacy ADV (z `event_type` 0x1x, `primary_phy=1M`), więc jeden skan
  zastępuje legacy scan trackera. Scan response dla legacy przychodzi jako osobny raport (0x1b/0x1a) – nasz
  komponent scala go z poprzedzającym ADV_IND (max 300 ms) tak jak `ble_device_base/scan_response_merger.h`
  robi to dla rp2/bk72xx (kod nie jest kompilowany na esp32 – `USE_BLE_SCAN_RESPONSE_MERGER`, więc własna
  mini-implementacja).
* Połączenia w LR (bonus, nie zaimplementowane): `esp_ble_gap_prefer_ext_connect_params_set()` (l. 4007),
  `esp_ble_gattc_aux_open()` (`esp_gattc_api.h:403`), `esp_ble_gap_set_preferred_default_phy()` (l. 3668).
* Kontroler ESP32-C3: `SOC_BLE_50_SUPPORTED`, Coded PHY S2/S8, ext adv/scan; `BT_CTRL_BLE_SCAN_DUPL`
  (filtr duplikatów działa tylko gdy `scan_duplicate=ENABLE` w parametrach – ustawiamy DISABLE),
  `BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM=100` (kontroler może odrzucać raporty gdy host nie nadąża).

## 3. pvvx ATC_MiThermometer (fw 5.9)

* `PVVX/app.h:115–116`: `cfg.flg2.bt5phy` (bit5, 0x20), `cfg.flg2.longrange` (bit6, 0x40, „сбрасывается после
  отключения питания” – kasowane po odłączeniu zasilania).
* `PVVX/ble.c:649–672` `init_ble()`: `adv_buf.ext_adv_init = EXT_ADV_Off`; gdy `bt5phy`:
  `blc_ll_init2MPhyCodedPhy_feature()`, `blc_ll_setDefaultConnCodingIndication(CODED_PHY_PREFER_S8)`, CSA#2; gdy
  dodatkowo `longrange`: `blc_ll_setDefaultExtAdvCodingIndication(ADV_HANDLE0, CODED_PHY_PREFER_S8)`,
  `ll_module_adv_cb = _blt_ext_adv_proc`, `adv_buf.ext_adv_init = EXT_ADV_Coded`,
  `blc_ll_setDefaultPhy(PHY_TRX_PREFER, BLE_PHY_CODED, BLE_PHY_CODED)`.
* `PVVX/ble.c:323–370` `ev_adv_timeout()`: w trybie Coded `blc_ll_setExtAdvParam(ADV_HANDLE0,
  ADV_EVT_PROP_EXTENDED_CONNECTABLE_UNDIRECTED, wrk.adv_interval, …, BLT_ENABLE_ADV_ALL, OWN_ADDRESS_PUBLIC, …,
  BLE_PHY_CODED /*primary*/, 0, BLE_PHY_CODED /*secondary*/, ADV_SID_0, 0)`; nazwa przez
  `blc_ll_setExtScanRspData()` (l. 362) i `bls_ll_setScanRspData()`; `blc_ll_setExtAdvEnable_1()` (l. 369).
  Oczekiwany raport: `event_type = 0x01` (connectable, nie-scannable, nie-legacy), `primary_phy = secondary_phy =
  Coded (3)`, `sid = 0`, `addr_type = public`, `data_status = complete`.
* `PVVX/ble.c:600–621` `load_adv_data()`: w trybie `EXT_ADV_Coded` **nazwa (0x09) jest dołączana do danych
  reklamowych** (`|| adv_buf.ext_adv_init == EXT_ADV_Coded`), flagi (0x01) gdy `cfg.flg2.adv_flags` (u nas
  flg2=0x70 → adv_flags=1), a całość idzie przez `blc_ll_setExtAdvData(…, DATA_OPER_COMPLETE, …)`. Skan pasywny na
  Coded wystarcza (pakiet i tak nie jest scannable).
* `PVVX/ble.c:387–405` `set_adv_con_time()`: po rozłączeniu na ~1 s `EXT_ADV_1M` (ext adv na 1M, legacy PDU
  `ADV_EVT_PROP_LEGACY_CONNECTABLE_SCANNABLE_UNDIRECTED`, l. 329–345) i powrót do Coded (`restore`).
* Reset LR: `PVVX/cmd_parser.h:62` `CMD_ID_LR_RESET = 0xDD`, `:45` `CMD_ID_CFG_DEF = 0x56` (obsługa
  `cmd_parser.c:880`, `:382`); README l. 138 (wyjęcie baterii → BT4.2), l. 144 (0xDD tylko LR).
* Format danych w LR = jak legacy: BTHome v2 service data 0xFCD2 (`bthome_beacon.c`: `bthome_data_beacon()`
  l. 222: info byte, 0x00 pid, 0x01 batt%, 0x02 temp(i16 ×0,01), 0x03 humi(u16 ×0,01), 0x0C vbat(mV) –
  wg parsera projektu-siostry `xiaomi_esp_flasher/xiaomi_device.cpp:73–184`), pvvx 0x181A (15 B), atc1441
  (13 B), Mi 0xFE95. Parser przeniesiony do `components/ble_longrange_scanner/adv_parser.*`.

## 4. Laboratorium (stan potwierdzony 22:18)
* `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_64:E8:33:86:FB:00-if00` → `/dev/ttyACM1`
  (root:dialout, brak `fuser`), flasher żyje na 192.168.0.102 (`/api/status`: uptime 884 s, heap 56 KB,
  `ble_state: scanning`), `A4:C1:38:4A:E8:8C` „Living Room” **offline od 22:05:11** (ostatni legacy adv), ostatnie
  wartości legacy: 23,25 °C / 63,08 % / 2911 mV; `last_error: BLE_TIMEOUT` (nieudany connect legacy do LR).
* Adapter PC `hci0` Intel 8087:0a2b HCI 4.2 – brak ext scan.
* Pozostałe termometry (legacy, BTHome v2): 0B:5F:4E, B0:E4:4A, AA:01:81, B8:31:E8, 93:72:DB, 3D:A8:7F.
