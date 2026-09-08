#pragma once
// BLE 5.0 extended scanner (LE 1M + LE Coded PHY / Long Range) for ESPHome on ESP32-C3/S3/C6 (Bluedroid).
//
// Design (see docs/architecture.md):
//  * hooks the raw Bluedroid GAP callback in front of esp32_ble's handler (esp_ble_gap_get_callback() +
//    esp_ble_gap_register_callback()) because ESPHome never queues ESP_GAP_BLE_EXT_ADV_REPORT_EVT;
//  * copies every extended advertising report into a lock-free pool/queue (BTC task -> main loop);
//  * runs one continuous extended scan (1M + Coded) that also delivers legacy advertisements, so the
//    esp32_ble_tracker must NOT start its own legacy scan; reports <= 62 bytes are forwarded to the tracker
//    (listeners, bluetooth_proxy) as a synthetic BLEScanResult;
//  * decodes pvvx / BTHome / atc1441 / Mi advertisements of configured devices into HA entities.

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/helpers.h"
#include "esphome/core/lock_free_queue.h"

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#ifdef USE_BLE_LR_TRACKER
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#endif

#include "adv_parser.h"

#include <esp_gap_ble_api.h>
#include <functional>
#include <vector>

namespace esphome::ble_longrange_scanner {

static constexpr uint8_t LR_MAX_ADV_DATA = 251;  // AUX_ADV_IND payload limit (esp_ble_gap_ext_adv_report_t)
static constexpr uint8_t LR_QUEUE_SIZE = 24;     // reports buffered between the BT task and the main loop

/// One extended advertising report, copied out of Bluedroid in the BT task.
struct LRReport {
  uint8_t event_type;  // bit0 connectable, bit1 scannable, bit2 directed, bit3 scan response, bit4 legacy PDU
  uint8_t addr_type;
  uint8_t addr[6];  // printable order (addr[0] = MSB), same as BLEScanResult::bda
  uint8_t primary_phy;    // ESP_BLE_GAP_PHY_1M / _CODED
  uint8_t secondary_phy;  // 0 = none, ESP_BLE_GAP_PHY_1M / _2M / _CODED
  uint8_t sid;
  int8_t tx_power;  // 127 = not available
  int8_t rssi;
  uint8_t data_status;  // 0 complete, 1 incomplete (more to come), 2 truncated
  uint8_t adv_data_len;
  uint8_t adv_data[LR_MAX_ADV_DATA];

  bool is_legacy() const { return this->event_type & 0x10; }
  bool is_scan_rsp() const { return this->event_type & 0x08; }
  bool is_scannable() const { return this->event_type & 0x02; }
  bool is_connectable() const { return this->event_type & 0x01; }
  bool is_coded() const { return this->primary_phy == ESP_BLE_GAP_PHY_CODED; }
  uint64_t address_uint64() const { return esp32_ble::ble_addr_to_uint64(this->addr); }
};

/// Queue element: either a scan-control event (status) or a report.
struct LREvent {
  enum Kind : uint8_t { REPORT, SET_PARAMS_COMPLETE, SCAN_START_COMPLETE, SCAN_STOP_COMPLETE, SCAN_TIMEOUT };
  Kind kind;
  uint8_t status;
  LRReport report;
  void release() {}  // required by EventPool (nothing heap-allocated)
};

/// A thermometer configured in YAML.
struct LRDevice {
  uint64_t mac{0};
  const char *name{""};
  sensor::Sensor *temperature{nullptr};
  sensor::Sensor *humidity{nullptr};
  sensor::Sensor *battery{nullptr};
  sensor::Sensor *battery_voltage{nullptr};
  sensor::Sensor *rssi{nullptr};
  sensor::Sensor *packet_counter{nullptr};
  text_sensor::TextSensor *phy{nullptr};
  text_sensor::TextSensor *format{nullptr};
  binary_sensor::BinarySensor *long_range{nullptr};
  // statistics
  uint32_t reports_1m{0};
  uint32_t reports_coded{0};
  uint32_t last_seen_ms{0};
  uint32_t first_seen_ms{0};
  uint8_t last_primary_phy{0};
  AdvFormat last_format{AdvFormat::UNKNOWN};

  void set_temperature(sensor::Sensor *s) { this->temperature = s; }
  void set_humidity(sensor::Sensor *s) { this->humidity = s; }
  void set_battery(sensor::Sensor *s) { this->battery = s; }
  void set_battery_voltage(sensor::Sensor *s) { this->battery_voltage = s; }
  void set_rssi(sensor::Sensor *s) { this->rssi = s; }
  void set_packet_counter(sensor::Sensor *s) { this->packet_counter = s; }
  void set_phy(text_sensor::TextSensor *s) { this->phy = s; }
  void set_format(text_sensor::TextSensor *s) { this->format = s; }
  void set_long_range(binary_sensor::BinarySensor *s) { this->long_range = s; }
};

enum class LRScanState : uint8_t {
  IDLE = 0,       // BLE stack not active / callback not hooked
  HOOKED,         // callback installed, scan not started
  SET_PARAMS,     // waiting for ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT
  STARTING,       // waiting for ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT
  RUNNING,        // extended scan active
  STOPPING,       // waiting for ESP_GAP_BLE_EXT_SCAN_STOP_COMPLETE_EVT
  FAILED,         // retry after backoff
};

const char *lr_scan_state_to_string(LRScanState s);
const char *lr_phy_to_string(uint8_t phy);

class BLELongRangeScanner : public Component, public Parented<esp32_ble::ESP32BLE> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // ---- configuration (from codegen) ----
  void set_scan_1m(uint16_t interval, uint16_t window, bool active) {
    this->interval_1m_ = interval;
    this->window_1m_ = window;
    this->active_1m_ = active;
  }
  void set_scan_coded(uint16_t interval, uint16_t window) {
    this->interval_coded_ = interval;
    this->window_coded_ = window;
  }
  /// Which PHYs to scan: bit0 = LE 1M (legacy advertisers), bit1 = LE Coded (Long Range).
  void set_phy_mask(uint8_t mask) { this->phy_mask_ = mask ? mask : 3; }
  void set_report_timeout(uint32_t ms) { this->report_timeout_ms_ = ms; }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ms_ = ms; }
  void set_forward_to_tracker(bool v) { this->forward_to_tracker_ = v; }
  void set_log_unknown(bool v) { this->log_unknown_ = v; }
  void set_coex_prefer_bt_on_boot(bool v) { this->coex_prefer_bt_ = v; }
  LRDevice *add_device(uint64_t mac, const char *name);
#ifdef USE_BLE_LR_TRACKER
  void set_tracker(esp32_ble_tracker::ESP32BLETracker *t) { this->tracker_ = t; }
#endif
  void set_reports_1m_sensor(sensor::Sensor *s) { this->reports_1m_sensor_ = s; }
  void set_reports_coded_sensor(sensor::Sensor *s) { this->reports_coded_sensor_ = s; }
  void set_free_heap_sensor(sensor::Sensor *s) { this->free_heap_sensor_ = s; }
  void set_scanner_state_text_sensor(text_sensor::TextSensor *s) { this->scanner_state_text_sensor_ = s; }
  void set_scanning_binary_sensor(binary_sensor::BinarySensor *s) { this->scanning_binary_sensor_ = s; }

  /// Other C++ code can subscribe to every (merged) report.
  void add_on_report_callback(std::function<void(const LRReport &)> &&cb) { this->report_callbacks_.push_back(std::move(cb)); }

  // ---- runtime API ----
  /// Change the scan parameters (milliseconds) and restart the scan – e.g. from an api action for tuning.
  void set_scan_ms(uint32_t interval_1m, uint32_t window_1m, bool active_1m, uint32_t interval_coded,
                   uint32_t window_coded, uint8_t phy_mask = 3);
  /// WiFi/BLE software coexistence: prefer Bluetooth (true) or balanced (false, IDF default).
  void set_coex_prefer_bt(bool prefer_bt);
  LRScanState get_scan_state() const { return this->state_; }
  bool is_scanning() const { return this->state_ == LRScanState::RUNNING; }
  void restart_scan();  // stop + start (e.g. from an automation)

 protected:
  // Raw Bluedroid GAP callback (BT task context). Handles the extended-scan events, chains the rest.
  static void gap_callback_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void enqueue_(LREvent::Kind kind, uint8_t status, const esp_ble_gap_ext_adv_report_t *rep);

  void hook_();
  void unhook_();
  void start_scan_();
  void stop_scan_(bool restart);
  void set_state_(LRScanState s);
  void fail_(const char *what, int status);

  void handle_event_(LREvent *ev);
  void handle_report_(LRReport &r);
  void deliver_(const LRReport &r, const uint8_t *adv, uint8_t adv_len, const uint8_t *rsp, uint8_t rsp_len);
  void publish_device_(LRDevice *dev, const LRReport &r, const AdvFields &f);
  void sweep_pending_(uint32_t now, bool flush_all);
  void log_stats_(uint32_t now);
  LRDevice *find_device_(uint64_t mac);

  // ---- fragment reassembly (data_status = incomplete) ----
  struct Fragment {
    bool used{false};
    uint8_t addr[6];
    uint8_t sid;
    uint16_t len;
    uint32_t stored_ms;
    uint8_t data[LR_MAX_ADV_DATA];
  };
  static constexpr size_t MAX_FRAGMENTS = 2;
  Fragment fragments_[MAX_FRAGMENTS];

  // ---- legacy ADV_IND + SCAN_RSP merge (the 4.2 Bluedroid path merges these, the 5.0 path does not) ----
  struct PendingAdv {
    bool used{false};
    uint32_t stored_ms;
    LRReport report;
  };
  static constexpr size_t MAX_PENDING = 6;
  static constexpr uint32_t PENDING_TIMEOUT_MS = 300;
  PendingAdv pending_[MAX_PENDING];
  uint8_t pending_count_{0};

  // ---- queue between the BT task and the main loop ----
  esphome::LockFreeQueue<LREvent, LR_QUEUE_SIZE> queue_;
  esphome::EventPool<LREvent, LR_QUEUE_SIZE - 1> pool_;  // one less than the queue: push() never fails

  // ---- configuration ----
  uint16_t interval_1m_{640}, window_1m_{128};       // 0.625 ms units: 400 ms / 80 ms
  uint16_t interval_coded_{640}, window_coded_{480};  // 400 ms / 300 ms
  bool active_1m_{true};
  uint8_t phy_mask_{3};  // ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK | ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK
  uint32_t report_timeout_ms_{120000};
  uint32_t stats_interval_ms_{60000};
  bool forward_to_tracker_{true};
  bool log_unknown_{false};
  bool coex_prefer_bt_{false};
  std::vector<LRDevice *> devices_;
  std::vector<std::function<void(const LRReport &)>> report_callbacks_;
#ifdef USE_BLE_LR_TRACKER
  esp32_ble_tracker::ESP32BLETracker *tracker_{nullptr};
  // ---- esp32_ble_tracker legacy-scan emulation ----
  // The tracker's esp_ble_gap_set_scan_params()/start_scanning()/stop_scanning() calls are redirected here at link
  // time (-Wl,--wrap, see __init__.py): the controller cannot run a legacy scan next to the extended one, so the
  // tracker gets synthetic completion events instead and believes it scans (RUNNING state, mode switching from HA
  // through bluetooth_proxy). The extended scan is the only real scan; its 1M part follows the tracker's
  // active/passive setting.
 public:
  void tracker_emulate_set_params(const esp_ble_scan_params_t *params);
  void tracker_emulate_start(uint32_t duration_s);
  void tracker_emulate_stop();

 protected:
  void tracker_emulation_loop_(uint32_t now);
  bool tracker_pending_params_{false};
  bool tracker_pending_start_{false};
  bool tracker_pending_stop_{false};
  bool tracker_emulated_running_{false};
  bool tracker_wants_active_{true};
  uint32_t tracker_scan_end_ms_{0};
  uint32_t tracker_duration_ms_{0};
#endif
  sensor::Sensor *reports_1m_sensor_{nullptr};
  sensor::Sensor *reports_coded_sensor_{nullptr};
  sensor::Sensor *free_heap_sensor_{nullptr};
  text_sensor::TextSensor *scanner_state_text_sensor_{nullptr};
  binary_sensor::BinarySensor *scanning_binary_sensor_{nullptr};

  // ---- state ----
  esp_gap_ble_cb_t prev_callback_{nullptr};
  bool hooked_{false};
  LRScanState state_{LRScanState::IDLE};
  uint32_t state_since_ms_{0};
  uint32_t retry_at_ms_{0};
  uint32_t retry_backoff_ms_{5000};
  bool restart_after_stop_{false};
  uint32_t last_report_ms_{0};
  uint32_t scan_started_ms_{0};
  uint32_t last_stats_ms_{0};

  // ---- statistics ----
  uint32_t total_reports_{0};
  uint32_t reports_1m_{0}, reports_coded_{0}, reports_2m_{0};
  uint32_t reports_legacy_{0}, reports_ext_{0}, reports_scan_rsp_{0};
  uint32_t stats_1m_prev_{0}, stats_coded_prev_{0};
  uint32_t fragments_incomplete_{0}, fragments_truncated_{0};
  uint32_t forwarded_{0}, not_forwarded_too_long_{0};
  uint32_t scan_restarts_{0}, scan_failures_{0};
  uint32_t dropped_{0};
  uint32_t min_free_heap_{0xFFFFFFFF};
  int8_t best_coded_rssi_{-128}, worst_coded_rssi_{0};
};

extern BLELongRangeScanner *global_lr_scanner;  // NOLINT

}  // namespace esphome::ble_longrange_scanner
