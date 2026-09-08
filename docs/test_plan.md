# Plan testów (sprzęt rzeczywisty)

| Test | Procedura | Kryterium |
|---|---|---|
| test_build_ble50 | `esphome compile` z `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y` | build OK, zapisz rozmiar flash/RAM |
| test_ext_scan_start | log komponentu po starcie | `SET_EXT_SCAN_PARAMS_COMPLETE status 0`, `EXT_SCAN_START_COMPLETE status 0` |
| test_legacy_still_visible | 2 min logu | raporty od 0B:5F:4E, B0:E4:4A, 93:72:DB … z `primary_phy=1M`, dane BTHome v2 zdekodowane |
| test_lr_report | 2 min logu | raporty od A4:C1:38:4A:E8:8C z `primary_phy=Coded` (secondary Coded), sid 0, RSSI, `adv_data_len` |
| test_lr_decode | porównaj z wartościami sprzed przełączenia (23,x °C / ~62 %) | temperatura/wilgotność/bateria sensowne, licznik pakietów rośnie co ~2,5 s |
| test_ha_entities | `aioesphomeapi` (wzór: `/home/mateusz/xiaomi_esp_flasher/scripts/ha_entities.py`) | encje Living Room z żywymi wartościami + diagnostyka PHY |
| test_stability_30min | 30 min, `/api` lub log | brak resetów, min. wolna sterta > 30 KB, ≥ 80 % oczekiwanych reklam LR |
| test_lr_reset | opcjonalnie: `0xDD` przez połączenie Coded albo wyjęcie baterii | termometr znów widoczny w legacy skanie flashera |
| test_restore_flasher | `scripts/flash_esp.sh` | flasher znów odpowiada na http://192.168.0.102/ |
