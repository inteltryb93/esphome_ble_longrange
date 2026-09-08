#pragma once
// Allocation-free parser for the advertising payloads of Xiaomi/Telink thermometers running pvvx
// ATC_MiThermometer (and the original Mi firmware). Port of
// xiaomi_esp_flasher/components/xiaomi_esp_flasher/xiaomi_device.cpp: parse_advertisement().

#include <cstddef>
#include <cstdint>

namespace esphome::ble_longrange_scanner {

enum class AdvFormat : uint8_t { UNKNOWN = 0, BTHOME_V2, BTHOME_V1, PVVX_CUSTOM, ATC1441, MIJIA };

const char *adv_format_to_string(AdvFormat f);

struct AdvMeasurement {
  AdvFormat format{AdvFormat::UNKNOWN};
  bool encrypted{false};
  bool valid{false};  // at least one measurement decoded (or an encrypted frame recognised)
  bool has_temperature{false};
  bool has_humidity{false};
  bool has_battery_pct{false};
  bool has_battery_mv{false};
  bool has_counter{false};
  float temperature{0};
  float humidity{0};
  uint8_t battery_pct{0};
  uint16_t battery_mv{0};
  uint8_t counter{0};
};

/// Decoded AD structures of one advertisement (adv data + optional scan response).
struct AdvFields {
  bool has_flags{false};
  uint8_t flags{0};
  uint8_t name_len{0};
  char name[32]{};
  uint16_t service_uuid{0};  // uuid16 of the service data that produced `meas`
  AdvMeasurement meas{};
};

/// Decode one service-data payload (`d`/`n` = bytes after the 16-bit UUID).
bool parse_service_data(uint16_t uuid16, const uint8_t *d, size_t n, AdvMeasurement &out);

/// Walk the AD structures in `data` and fill `out` (first decodable service data wins).
void parse_ad_structures(const uint8_t *data, size_t len, AdvFields &out);

}  // namespace esphome::ble_longrange_scanner
