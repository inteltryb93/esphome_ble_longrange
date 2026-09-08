# esphome_ble_longrange

Komponent zewnętrzny ESPHome `ble_longrange_scanner`: **skan rozszerzony BLE 5.0 (LE 1M + LE Coded PHY / Long
Range)** na ESP32-C3 (Bluedroid, ESP-IDF 5.5.5), dzięki któremu termometry Xiaomi LYWSD03MMC z firmware pvvx
w trybie „BT5+ PHY + LE Long Range” (Advertising Extensions, Coded PHY S=8) są widoczne i dekodowane do encji
Home Assistant (temperatura, wilgotność, bateria, napięcie, RSSI, PHY, „Long Range”). Standardowy
`esp32_ble_tracker` (legacy scan) takich termometrów nie widzi. Termometry legacy są odbierane tym samym skanem
(PHY 1M) i przekazywane do `esp32_ble_tracker` / `bluetooth_proxy`.

```
components/ble_longrange_scanner/   komponent (Python + C++), parser reklam (BTHome v2 / pvvx / atc1441 / Mi)
examples/bluetooth_proxy_longrange.yaml   przykład: zwykłe Bluetooth proxy (tylko reklamy) + Long Range (source: github)
examples/longrange_sensors.yaml           przykład: encje per termometr, statystyki, usługi API do strojenia
yaml/bluetooth_proxy_lr.yaml        moja konfiguracja laboratoryjna (source: local) – to samo co przykład proxy
yaml/ble_longrange.yaml             moja konfiguracja diagnostyczna (source: local)
docs/research.md                    ustalenia z kodu ESPHome / ESP-IDF / pvvx (z odwołaniami do linii)
docs/architecture.md                co reużyto z ESPHome, co napisano od zera, przepływ danych, ograniczenia
docs/test_report.md                 wyniki na prawdziwym sprzęcie (logi w logs/)
docs/facts.md, docs/test_plan.md    zebrane fakty źródłowe (pvvx / ESP-IDF / ESPHome) i plan testów
scripts/capture.sh                  zapis logu z API na N s + podsumowanie (scripts/analyze_log.py)
scripts/ha_entities.py              lista encji i stanów przez natywne API (to, co widzi HA)
scripts/sweep.py, sweep_coded.py    przegląd parametrów skanu w locie (usługi API set_scan / set_coex), 1M+Coded / tylko Coded
scripts/proxy_check.py              strumień surowych reklam bluetooth_proxy (to, co dostaje integracja Bluetooth HA)
scripts/stability_poll.sh           odpytywanie encji diagnostycznych co 5 min (test stabilności)
scripts/set_longrange.sh            włączenie LR przez API flashera (termometr w legacy) / reset_longrange.sh: powrót
```

## Jak to działa (skrót)

* ESPHome nigdy nie kolejkuje `ESP_GAP_BLE_EXT_ADV_REPORT_EVT`, a jego handler GAP jest chroniony. Komponent
  pobiera dotychczasowy callback (`esp_ble_gap_get_callback()`), rejestruje własny i **łańcuchuje** wszystko,
  czego sam nie obsługuje, do ESPHome – bez zmian w `/home/mateusz/esphome`.
* W zadaniu BT raport jest kopiowany do puli (23 × 268 B) i kolejki lock-free; pętla główna dekoduje, publikuje
  encje i (opcjonalnie) podaje ramkę trackerowi jako `BLEScanResult` (`gap_scan_event_handler`), z czego korzysta
  `bluetooth_proxy` i wszystkie platformy sensorów BLE ESPHome.
* Jeden ciągły ext scan (`esp_ble_gap_set_ext_scan_params` z maską 1M|Coded, `esp_ble_gap_start_ext_scan(0,0)`),
  watchdog braku raportów, ponowienia z backoffem, scalanie fragmentów i legacy ADV+SCAN_RSP.
* sdkconfig: `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y`, `CONFIG_BT_BLE_50_EXTEND_SCAN_EN=y` (ESPHome domyślnie
  wyłącza 5.0); BLE 4.2 zostaje włączone, żeby tracker/klient nadal się kompilowały.

## Konfiguracja – tryb Bluetooth proxy (`examples/bluetooth_proxy_longrange.yaml`)

Dokładnie jak zwykłe proxy ESPHome (`esp32_ble_tracker` + `bluetooth_proxy: active: false`) plus blok
`ble_longrange_scanner:`:

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
  active: false               # tylko reklamy (połączenia GATT nie działają obok ext scanu)

ble_longrange_scanner:        # podmienia skan legacy trackera na ext scan 1M + Coded PHY
  scan_parameters:
    interval: 400ms
    window: 20ms              # 1M: małe okno, urządzenia legacy nadal widoczne
    coded_interval: 400ms
    coded_window: 380ms       # Coded: okno ≈ interwał (zmierzone optimum)
```

Wymagania: ESP32-C3/S3/C6/C5/H2 (kontroler BLE 5.0; klasyczny ESP32 i S2 mają tylko BLE 4.2 – konfiguracja jest
odrzucana z czytelnym błędem), framework `esp-idf`, ESPHome 2026.1+.

W Home Assistant urządzenie wygląda i zachowuje się jak każde Bluetooth proxy: zero encji, stan skanera RUNNING,
tryb pasywny/aktywny sterowany z HA, reklamy (legacy i Coded PHY) trafiają do integracji Bluetooth / BTHome.
Wywołania `esp_ble_gap_set_scan_params/start_scanning/stop_scanning` trackera są przekierowane na etapie
linkowania (`-Wl,--wrap`) do komponentu, który emuluje zdarzenia zakończenia – tracker „myśli”, że skanuje,
a jedynym prawdziwym skanem jest rozszerzony (kontroler C3 nie pozwala na oba naraz).

Opcje komponentu (wszystkie opcjonalne):

```yaml
ble_longrange_scanner:
  scan_parameters:
    interval: 400ms           # LE 1M
    window: 80ms
    active: true              # nadpisywane przez tryb trackera/HA (proxy active: false -> pasywny)
    coded_interval: 400ms     # LE Coded
    coded_window: 300ms
    phy: both                 # both | 1m | coded  – "coded": tylko Long Range, bez urządzeń legacy
  report_timeout: 120s        # brak raportów -> restart skanu
  stats_interval: 60s         # linia statystyk w logu (INFO)
  forward_to_tracker: true    # raporty -> tracker -> bluetooth_proxy / platformy sensorów BLE
  coex_prefer_bt: true        # esp_coex_preference_set(ESP_COEX_PREFER_BT): +60 % odbioru Coded
  log_unknown_devices: false
  # wariant diagnostyczny (yaml/ble_longrange.yaml): encje per termometr i statystyki
  reports_1m: {name: BLE Reports 1M}
  reports_coded: {name: BLE Reports Coded}
  free_heap: {name: Free Heap}
  scanner_state: {name: Scanner State}
  scanning: {name: Scanning}
  devices:
    - mac_address: "A4:C1:38:4A:E8:8C"
      name: Living Room       # encje: <name> Temperature/Humidity/Battery/Battery Voltage/RSSI/Packet Counter,
                              #        <name> PHY, <name> Advertising Format (text), <name> Long Range (binary)
      packet_counter: false   # dowolną encję można wyłączyć (false) albo nadpisać (name/filters/...)
```

## Build / flash / testy

```bash
ESPHOME=/home/mateusz/xiaomi_esp_flasher/.venv/bin/esphome        # edytowalna instalacja checkoutu 2026.10.0-dev
cd /home/mateusz/esphome_ble_longrange
$ESPHOME compile yaml/bluetooth_proxy_lr.yaml
$ESPHOME run --no-logs --device 192.168.0.102 yaml/bluetooth_proxy_lr.yaml   # OTA (albo --device /dev/ttyACM1)
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/proxy_check.py 192.168.0.102 120 A4:C1:38:4A:E8:8C
scripts/capture.sh 300 run                                              # log VERY_VERBOSE 5 min + statystyki
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/ha_entities.py 192.168.0.102 10
/home/mateusz/xiaomi_esp_flasher/.venv/bin/python scripts/sweep.py 192.168.0.102 3   # strojenie okien skanu
```

Powrót do flashera: `cd /home/mateusz/xiaomi_esp_flasher && scripts/flash_esp.sh`.

**Uwaga (dotyczy każdego proxy ESPHome):** `bluetooth_proxy` ma jedno miejsce subskrybenta reklam – „najnowszy
wygrywa”. `scripts/proxy_check.py` (albo inny klient `aioesphomeapi` subskrybujący reklamy) odbiera je Home
Assistantowi, a HA nie zapisuje się ponownie samo: encje BTHome zamierają do restartu ESP32 (ponowny upload
firmware / odłączenie zasilania). Nie uruchamiaj tego skryptu, gdy HA jest połączone, albo zrestartuj potem ESP32.

## Home Assistant

Urządzenie `bluetooth-proxy-lr` (API, mDNS) pojawia się w HA jako wykryta integracja ESPHome: *Ustawienia →
Urządzenia i usługi → Wykryte → ESPHome „bluetooth-proxy-lr” → Konfiguruj*. Po dodaniu HA używa go jako
Bluetooth proxy: termometr w trybie Long Range pojawia się w integracji BTHome tak jak każdy inny (identyczne
dane BTHome v2, adres publiczny).

## Powrót termometru z Long Range do legacy

Flaga LR nie jest zapisywana trwale: **wyjęcie i włożenie baterii** przywraca BT 4.2 (pvvx README l. 138).
Alternatywnie komenda `0xDD` (CMD_ID_LR_RESET) albo `0x56` (defaults) na charakterystyce 0x1F1F przez połączenie
Coded PHY (nRF Connect na telefonie z BT5; ten komponent nie nawiązuje połączeń). Potem
`scripts/set_longrange.sh <ip-flashera> <MAC>` włącza LR ponownie (wymaga firmware flashera na ESP32).

## Odbiór pakietów Long Range – co go ogranicza

Zmierzone (`docs/test_report.md` §5, §8d, §8e): przy dobrym sygnale (termometr blisko, bez WiFi) firmware odbiera
**98–100 %** zdarzeń reklamowych Coded – host, kolejka i Bluedroid nic nie gubią. Przy słabym sygnale
(−85…−95 dBm) odbiór spada do 50–70 % bez WiFi i 18–30 % z WiFi + HA: straty to zaniki poniżej czułości Coded S8
(−104 dBm) oraz czas radia zabierany przez WiFi. Parametry skanu są już optymalne (okno Coded = interwał, 1M 20 ms
albo `phy: coded`); opcje kontrolera (AGC recorrect, flow control, TLIM) nie zmieniają wyniku. Co realnie pomaga:
lepsze położenie/antena ESP32, większa moc nadawania termometru (pvvx `rf_tx_power`, do +10 dBm), mniej ruchu WiFi,
drugie proxy bliżej termometru.

Wyniki pomiarów, ograniczenia i statystyki odbioru: `docs/test_report.md`.
