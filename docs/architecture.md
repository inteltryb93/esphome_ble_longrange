# Architektura komponentu `ble_longrange_scanner`

## Co jest reużyte z ESPHome

| Potrzeba | ESPHome | Uwagi |
|---|---|---|
| Kontroler + Bluedroid, cykl życia stosu | `esp32_ble` (`ESP32BLE`, `Parented`) | komponent czeka w `loop()` aż `parent->is_active()`; przy `disable()`/`enable()` stosu ponownie się podpina (Bluedroid po re-init ma znów callback ESPHome). |
| Kolejka zdarzeń BT-task → main loop | `esphome/core/event_pool.h` + `lock_free_queue.h` | identyczny wzorzec jak `ESP32BLE::ble_events_`; własna pula 24 slotów × ~270 B (raport ext ma do 251 B danych, `BLEEvent` ESPHome ich nie mieści). |
| Encje HA | `sensor`, `text_sensor`, `binary_sensor` | tworzone w Pythonie z listy `devices:`; domyślne nazwy `<name> Temperature` itd. |
| Adres MAC / logowanie | `format_mac_addr_upper`, `ESP_LOGx` | tagi `ble_lr` (`ble_lr.scan`, `ble_lr.dev`). |
| Zgodność z resztą ekosystemu BLE (`bluetooth_proxy`, platformy sensorów, triggery) | `esp32_ble_tracker::gap_scan_event_handler(const BLEScanResult&)` (publiczne) | **forwarding**: każdy raport ext (legacy 1M i Coded) o długości ≤ 62 B jest zamieniany na `BLEScanResult` i podany trackerowi, który dalej sam obsługuje listenerów i proxy. **Emulacja skanu trackera**: jego wywołania `esp_ble_gap_set_scan_params/start_scanning/stop_scanning` są przekierowane linkerem (`-Wl,--wrap`) do komponentu, który z pętli głównej dostarcza trackerowi syntetyczne `SCAN_PARAM_SET/START/STOP_COMPLETE` (status 0) przez publiczne `gap_event_handler()` oraz `ESP_GAP_SEARCH_INQ_CMPL_EVT` po `duration` – tracker przechodzi normalnie IDLE→STARTING→RUNNING→IDLE, `bluetooth_proxy` raportuje HA stan RUNNING i tryb, a `active/passive` trackera (z YAML albo z HA) steruje częścią 1M ext scanu. Zmierzone: kontroler C3 odrzuca (status 12, Command Disallowed) ext scan gdy trwa legacy i odwrotnie. |
| sdkconfig | `esp32.add_idf_sdkconfig_option` | `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_EXTEND_SCAN_EN=y`; BLE 4.2 zostaje `y` (domyślne ESPHome) żeby tracker/klient nadal się kompilowały. |

## Co jest napisane od zera i dlaczego

1. **Przechwycenie callbacku GAP** (`ble_longrange_scanner.cpp: gap_callback_`): ESPHome nie kolejkuje
   `ESP_GAP_BLE_EXT_ADV_REPORT_EVT` ani `*_EXT_SCAN_*_COMPLETE_EVT` (patrz `docs/research.md` §1.1), a jego
   `gap_event_handler` jest chroniony. Komponent robi `prev = esp_ble_gap_get_callback();
   esp_ble_gap_register_callback(own)`; własny callback (kontekst BTC) obsługuje 5 zdarzeń ext scan (kopia do puli
   + push do kolejki), wszystko inne oddaje do `prev` – ESPHome nie widzi różnicy. Zero zmian w
   `/home/mateusz/esphome`.
2. **Maszyna stanów ext scan**: `IDLE → HOOKED → PARAMS_SET(wait) → STARTING(wait) → RUNNING`, błędy → `FAILED`
   z ponowieniem po 5 s (backoff do 60 s); `duration=0, period=0` = skan ciągły (bez cyklicznego restartu jak w
   trackerze). Watchdog: brak raportów przez `report_timeout` (domyślnie 120 s) → `stop_ext_scan` → restart.
3. **Parametry**: `cfg_mask = 1M | Coded`; 1M: aktywny/pasywny wg YAML, Coded: pasywny (pvvx w LR nie jest
   scannable, nazwa jest w AUX_ADV_IND); interval/window osobno per PHY (domyślnie 1M 400/80 ms, Coded 400/300 ms –
   suma okien < interval, bo kontroler przeplata PHY); `scan_duplicate = DISABLE`, `filter_policy = ALLOW_ALL`,
   `own_addr_type = PUBLIC`.
4. **Scalanie fragmentów** (`data_status = incomplete`) per adres+SID (2 sloty × 251 B) i **scalanie legacy ADV +
   SCAN_RSP** (adv scannable trzymany ≤ 300 ms, jak `scan_response_merger.h` dla rp2/bk72xx, którego esp32 nie
   kompiluje) – dzięki temu tracker/proxy dostają jedną ramkę jak z natywnego skanu 4.2.
5. **Parser reklam** (`adv_parser.*`, bez alokacji): AD-structures → flags/nazwa/service data; formaty BTHome v2
   (0xFCD2), pvvx custom (0x181A/15 B), atc1441 (0x181A/13 B), Mi (0xFE95) – port z
   `xiaomi_esp_flasher/xiaomi_device.cpp: parse_advertisement()`.
6. **Urządzenia i encje**: tablica `Device{mac, name, sensors…, stats}`; na każdy raport z dopasowanym MAC:
   publikacja temperatury/wilgotności/baterii/napięcia/RSSI/licznika pakietów, `text_sensor` PHY
   („1M”, „2M”, „Coded”), `binary_sensor` Long Range (primary PHY = Coded), `text_sensor` format. Liczniki
   per urządzenie (legacy/coded) do statystyk odbioru.
7. **Emulacja skanu legacy trackera** (`__wrap_esp_ble_gap_*` w `ble_longrange_scanner.cpp`, flagi linkera w
   `__init__.py`): jedyni wywołujący te API w buildzie ESPHome to `esp32_ble_tracker`; `__real_*` pozostają
   dostępne (użyte, gdy komponent nie jest jeszcze zainicjalizowany). Dzięki temu YAML jest identyczny ze
   zwykłym Bluetooth proxy (bez `continuous: false`).
8. **Strojenie w locie**: `set_scan_ms()` / `set_coex_prefer_bt()` / `restart_scan()` wystawione w YAML jako
   `api: actions:` (`set_scan`, `set_coex`, `restart_scan`) – `scripts/sweep.py` przełącza parametry przez
   natywne API i czyta sensory `reports_1m` / `reports_coded`, bez reflashowania i bez strumienia logów (ruch WiFi
   zaburza pomiar przez koegzystencję). Opcja `coex_prefer_bt` ustawia `esp_coex_preference_set(ESP_COEX_PREFER_BT)`
   (ten sam mechanizm, którego `esp32_ble_tracker` używa na czas połączeń GATT).
9. **Statystyki i diagnostyka** (`ble_lr` INFO co `stats_interval`, domyślnie 60 s): raporty/min per PHY,
   dropy kolejki, restarty skanu, wolna sterta / min sterta, `esp_reset_reason()`. Opcjonalne encje globalne:
   `reports_1m`, `reports_coded`, `free_heap`, `scanner_state`.

## Przepływ danych

```
kontroler C3 ─HCI LE Ext Adv Report─▶ Bluedroid (btu → btm → btc) ─▶ gap_callback_ [BTC task]
   ├─ ext scan events  → EventPool/LockFreeQueue ─▶ loop() [main]
   └─ pozostałe        → ESP32BLE::gap_event_handler (oryginalny) → kolejka ESPHome
loop(): pop → (fragmenty) → (merge scan rsp) → handle_report_()
   ├─ Device match → adv_parser → sensor::publish_state … (HA przez api)
   └─ tracker_ → gap_scan_event_handler(BLEScanResult) → listenery / bluetooth_proxy → HA raw adv
```

## Ograniczenia zmierzone (docs/test_report.md §5)
* Odbiór reklam Coded PHY: 5–8 z 24 zdarzeń/min (22–35 %) niezależnie od okien 1M/Coded, także w trybie
  `phy: coded` (bez 1M) i przy 100 % okna Coded; `coex_prefer_bt` daje +60 % przy identycznych oknach w trybie
  1M+Coded. Sufit ≈ 1/3 odpowiada skanerowi nasłuchującemu na jednym kanale primary na interwał przy adwerterze
  wysyłającym ADV_EXT_IND (Coded) skutecznie na jednym kanale na zdarzenie – host nie gubi nic (`dropped=0`).

## Ograniczenia znane z góry
* Raporty > 62 B nie są przekazywane do trackera/proxy (limit `BLEScanResult`/`BluetoothLERawAdvertisement`);
  encje własne działają dla pełnych 251 B.
* Po użyciu ext scan kontroler odrzuca komendy legacy: `ble_client`/`bluetooth_proxy` z aktywnymi połączeniami
  (`esp_ble_gattc_open` = legacy create connection) nie są wspierane w tym buildzie; `bluetooth_proxy` tylko w
  trybie `active: false` (same reklamy). Połączenie w LR wymagałoby `esp_ble_gattc_aux_open()` (bonus).
* Coded S2 vs S8 nie jest rozróżniane bez `CONFIG_BT_BLE_FEAT_ADV_CODING_SELECTION` (pvvx nadaje S8).
