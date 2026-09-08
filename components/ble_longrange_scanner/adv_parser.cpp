#include "adv_parser.h"

#include <cstring>

namespace esphome::ble_longrange_scanner {

static inline int16_t i16le(const uint8_t *p) { return (int16_t) (p[0] | (p[1] << 8)); }
static inline uint16_t u16le(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

const char *adv_format_to_string(AdvFormat f) {
  switch (f) {
    case AdvFormat::BTHOME_V2:
      return "BTHome v2";
    case AdvFormat::BTHOME_V1:
      return "BTHome v1";
    case AdvFormat::PVVX_CUSTOM:
      return "pvvx";
    case AdvFormat::ATC1441:
      return "atc1441";
    case AdvFormat::MIJIA:
      return "Mi";
    default:
      return "unknown";
  }
}

bool parse_service_data(uint16_t uuid16, const uint8_t *d, size_t n, AdvMeasurement &out) {
  if (uuid16 == 0xFCD2) {  // BTHome v2 (pvvx bthome_beacon.c): byte0 = device info (bit0 encryption, bits5-7 version)
    if (n < 1)
      return false;
    out.format = AdvFormat::BTHOME_V2;
    out.encrypted = d[0] & 0x01;
    if (out.encrypted) {
      out.valid = true;
      return true;
    }
    size_t i = 1;
    while (i < n) {
      uint8_t id = d[i++];
      switch (id) {
        case 0x00:  // packet id
          if (i + 1 > n)
            return true;
          out.counter = d[i];
          out.has_counter = true;
          i += 1;
          break;
        case 0x01:  // battery %
          if (i + 1 > n)
            return true;
          out.battery_pct = d[i];
          out.has_battery_pct = true;
          i += 1;
          break;
        case 0x02:  // temperature 0.01 °C
          if (i + 2 > n)
            return true;
          out.temperature = i16le(d + i) / 100.0f;
          out.has_temperature = true;
          i += 2;
          break;
        case 0x03:  // humidity 0.01 %
          if (i + 2 > n)
            return true;
          out.humidity = u16le(d + i) / 100.0f;
          out.has_humidity = true;
          i += 2;
          break;
        case 0x0C:  // voltage 0.001 V
          if (i + 2 > n)
            return true;
          out.battery_mv = u16le(d + i);
          out.has_battery_mv = true;
          i += 2;
          break;
        case 0x2E:  // humidity 1 %
          if (i + 1 > n)
            return true;
          out.humidity = d[i];
          out.has_humidity = true;
          i += 1;
          break;
        case 0x45:  // temperature 0.1 °C
          if (i + 2 > n)
            return true;
          out.temperature = i16le(d + i) / 10.0f;
          out.has_temperature = true;
          i += 2;
          break;
        case 0x10:
        case 0x11:
        case 0x0F:
        case 0x15:
        case 0x16:
        case 0x21:  // binary sensors
        case 0x3A:  // button event
          i += 1;
          break;
        case 0x3C:
        case 0x3D:
          i += 2;
          break;
        case 0x3E:
        case 0x50:
          i += 4;
          break;
        default:  // unknown object: length unknown, stop parsing
          out.valid = out.has_temperature || out.has_humidity || out.has_battery_pct || out.has_battery_mv;
          return true;
      }
    }
    // pvvx alternates two BTHome frames: temperature/humidity/battery% and voltage(+binary objects)
    out.valid = out.has_temperature || out.has_humidity || out.has_battery_pct || out.has_battery_mv;
    return true;
  }
  if (uuid16 == 0x181A) {
    if (n == 15) {  // pvvx custom: mac[6] LE, temp i16 x0.01, humi u16 x0.01, vbat u16 mV, bat u8, cnt u8, flags u8
      out.format = AdvFormat::PVVX_CUSTOM;
      out.temperature = i16le(d + 6) / 100.0f;
      out.humidity = u16le(d + 8) / 100.0f;
      out.battery_mv = u16le(d + 10);
      out.battery_pct = d[12];
      out.counter = d[13];
      out.has_temperature = out.has_humidity = out.has_battery_mv = out.has_battery_pct = out.has_counter = true;
      out.valid = true;
      return true;
    }
    if (n == 13) {  // atc1441: mac[6] BE, temp i16 BE x0.1, humi u8, bat u8, vbat u16 BE, cnt u8
      out.format = AdvFormat::ATC1441;
      out.temperature = (int16_t) ((d[6] << 8) | d[7]) / 10.0f;
      out.humidity = d[8];
      out.battery_pct = d[9];
      out.battery_mv = (d[10] << 8) | d[11];
      out.counter = d[12];
      out.has_temperature = out.has_humidity = out.has_battery_mv = out.has_battery_pct = out.has_counter = true;
      out.valid = true;
      return true;
    }
    if (n == 11 || n == 19) {  // pvvx custom encrypted
      out.format = AdvFormat::PVVX_CUSTOM;
      out.encrypted = true;
      out.valid = true;
      return true;
    }
    return false;
  }
  if (uuid16 == 0x181C) {
    out.format = AdvFormat::BTHOME_V1;
    out.valid = true;
    return true;
  }
  if (uuid16 == 0xFE95) {  // MiBeacon
    if (n < 5)
      return false;
    out.format = AdvFormat::MIJIA;
    uint16_t ctrl = u16le(d);
    size_t i = 2;
    if ((ctrl & 0xf000) < 0x2000)
      return false;
    out.counter = d[i + 2];
    out.has_counter = true;
    i += 3;
    if (ctrl & 0x10)
      i += 6;  // MAC
    if (ctrl & 0x20) {  // capability
      if (i >= n)
        return true;
      uint8_t cap = d[i++];
      if (cap & 0x20)
        i += 2;
    }
    if (ctrl & 0x40) {
      if (ctrl & 8) {
        out.encrypted = true;
        out.valid = true;
        return true;
      }
      if (i + 3 > n)
        return true;
      uint16_t data_id = u16le(d + i);
      uint8_t dl = d[i + 2];
      i += 3;
      if (i + dl > n)
        return true;
      switch (data_id) {
        case 0x1004:
          if (dl == 2) {
            out.temperature = i16le(d + i) / 10.0f;
            out.has_temperature = true;
          }
          break;
        case 0x1006:
          if (dl == 2) {
            out.humidity = i16le(d + i) / 10.0f;
            out.has_humidity = true;
          }
          break;
        case 0x100A:
          if (dl >= 1) {
            out.battery_pct = d[i];
            out.has_battery_pct = true;
          }
          break;
        case 0x100D:
          if (dl == 4) {
            out.temperature = i16le(d + i) / 10.0f;
            out.humidity = i16le(d + i + 2) / 10.0f;
            out.has_temperature = out.has_humidity = true;
          }
          break;
        default:
          break;
      }
      out.valid = true;
    }
    return true;
  }
  return false;
}

void parse_ad_structures(const uint8_t *data, size_t len, AdvFields &out) {
  size_t i = 0;
  while (i + 1 < len) {
    uint8_t l = data[i];
    if (l == 0) {
      i++;
      continue;
    }
    if (i + 1 + l > len)
      break;  // malformed / truncated structure
    uint8_t type = data[i + 1];
    const uint8_t *p = data + i + 2;
    size_t n = l - 1;
    switch (type) {
      case 0x01:  // Flags
        if (n >= 1) {
          out.has_flags = true;
          out.flags = p[0];
        }
        break;
      case 0x08:  // shortened local name
      case 0x09:  // complete local name
        if (type == 0x09 || out.name_len == 0) {
          size_t c = n < sizeof(out.name) - 1 ? n : sizeof(out.name) - 1;
          memcpy(out.name, p, c);
          out.name[c] = '\0';
          out.name_len = (uint8_t) c;
        }
        break;
      case 0x16:  // service data, 16-bit UUID
        if (n >= 2 && !out.meas.valid) {
          uint16_t uuid = u16le(p);
          AdvMeasurement m{};
          if (parse_service_data(uuid, p + 2, n - 2, m) && (m.valid || m.format != AdvFormat::UNKNOWN)) {
            out.meas = m;
            out.service_uuid = uuid;
          }
        }
        break;
      default:
        break;
    }
    i += 1 + l;
  }
}

}  // namespace esphome::ble_longrange_scanner
