# Test plan (real hardware)

| Test | Procedure | Criterion |
|---|---|---|
| test_build_ble50 | `esphome compile` with `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y` | build OK, record flash/RAM size |
| test_ext_scan_start | component log after boot | `SET_EXT_SCAN_PARAMS_COMPLETE status 0`, `EXT_SCAN_START_COMPLETE status 0` |
| test_legacy_still_visible | 2 min of log | reports from 0B:5F:4E, B0:E4:4A, 93:72:DB … with `primary_phy=1M`, BTHome v2 decoded |
| test_lr_report | 2 min of log | reports from A4:C1:38:4A:E8:8C with `primary_phy=Coded` (secondary Coded), sid 0, RSSI, `adv_data_len` |
| test_lr_decode | compare with the values before the switch (23.x °C / ~62 %) | temperature/humidity/battery plausible, packet counter advancing every ~2.5 s |
| test_ha_entities | `aioesphomeapi` (template: `/home/mateusz/xiaomi_esp_flasher/scripts/ha_entities.py`) | Living Room entities with live values + PHY diagnostics |
| test_stability_30min | 30 min, `/api` or log | no resets, minimum free heap > 30 KB, ≥ 80 % of the expected LR advertisements |
| test_lr_reset | optional: `0xDD` over a Coded connection or a battery pull | thermometer visible again in the flasher's legacy scan |
| test_restore_flasher | `scripts/flash_esp.sh` | flasher answers again on http://192.168.0.102/ |
