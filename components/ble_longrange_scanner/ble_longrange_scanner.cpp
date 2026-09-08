#include "ble_longrange_scanner.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_bt_defs.h>
#include <esp_system.h>
#ifdef USE_ESP32_BLE_SOFTWARE_COEXISTENCE
#include <esp_coexist.h>
#endif
#include <sdkconfig.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string>

// The report struct names its secondary-PHY field differently depending on the PAwR feature (esp_gap_ble_api.h).
#if defined(CONFIG_BT_BLE_FEAT_PAWR_EN) && CONFIG_BT_BLE_FEAT_PAWR_EN
#define LR_SECONDARY_PHY(rep) ((rep)->secondary_phy)
#else
#define LR_SECONDARY_PHY(rep) ((rep)->secondly_phy)
#endif

namespace esphome::ble_longrange_scanner {

static const char *const TAG = "ble_lr";
static const char *const TAG_SCAN = "ble_lr.scan";
static const char *const TAG_DEV = "ble_lr.dev";

BLELongRangeScanner *global_lr_scanner = nullptr;  // NOLINT

static constexpr uint8_t DATA_STATUS_COMPLETE = 0x00;
static constexpr uint8_t DATA_STATUS_INCOMPLETE = 0x01;
static constexpr uint8_t DATA_STATUS_TRUNCATED = 0x02;
static constexpr uint32_t CONTROL_EVENT_TIMEOUT_MS = 10000;
static constexpr uint32_t MAX_BACKOFF_MS = 60000;

const char *lr_scan_state_to_string(LRScanState s) {
  switch (s) {
    case LRScanState::IDLE:
      return "IDLE";
    case LRScanState::HOOKED:
      return "HOOKED";
    case LRScanState::SET_PARAMS:
      return "SET_PARAMS";
    case LRScanState::STARTING:
      return "STARTING";
    case LRScanState::RUNNING:
      return "RUNNING";
    case LRScanState::STOPPING:
      return "STOPPING";
    case LRScanState::FAILED:
      return "FAILED";
    default:
      return "?";
  }
}

const char *lr_phy_to_string(uint8_t phy) {
  switch (phy) {
    case 0:
      return "none";
    case ESP_BLE_GAP_PHY_1M:
      return "1M";
    case ESP_BLE_GAP_PHY_2M:
      return "2M";
    case ESP_BLE_GAP_PHY_CODED:
      return "Coded";
    case 4:
      return "Coded S2";
    default:
      return "?";
  }
}

static const char *reset_reason_to_string(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external pin";
    case ESP_RST_SW:
      return "software (esp_restart)";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

// ---------------------------------------------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------------------------------------------

void BLELongRangeScanner::setup() {
  global_lr_scanner = this;
  if (!this->pool_.warm()) {
    ESP_LOGE(TAG, "Could not pre-allocate the report pool (%u x %u B)", LR_QUEUE_SIZE - 1, (unsigned) sizeof(LREvent));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Boot: reset reason %d (%s), free heap %" PRIu32 " B, report pool %u x %u B",
           (int) esp_reset_reason(), reset_reason_to_string(esp_reset_reason()), esp_get_free_heap_size(),
           LR_QUEUE_SIZE - 1, (unsigned) sizeof(LREvent));
  this->last_stats_ms_ = millis();
  if (this->scanner_state_text_sensor_ != nullptr)
    this->scanner_state_text_sensor_->publish_state(lr_scan_state_to_string(this->state_));
  if (this->scanning_binary_sensor_ != nullptr)
    this->scanning_binary_sensor_->publish_state(false);
}

LRDevice *BLELongRangeScanner::add_device(uint64_t mac, const char *name) {
  auto *d = new LRDevice();  // NOLINT
  d->mac = mac;
  d->name = name;
  this->devices_.push_back(d);
  return d;
}

void BLELongRangeScanner::loop() {
  const uint32_t now = millis();

  if (!this->parent_->is_active()) {
    if (this->hooked_)
      this->unhook_();
    return;
  }
  if (!this->hooked_) {
    if (now < this->retry_at_ms_)
      return;
    this->hook_();
    if (!this->hooked_)
      return;
  }

  // Drain the BT-task queue.
  LREvent *ev;
  while ((ev = this->queue_.pop()) != nullptr) {
    this->handle_event_(ev);
    this->pool_.release(ev);
  }
  uint16_t dropped = this->queue_.get_and_reset_dropped_count();
  if (dropped) {
    this->dropped_ += dropped;
    ESP_LOGW(TAG, "Dropped %u reports (queue full)", dropped);
  }

  // Scan state machine timers.
  switch (this->state_) {
    case LRScanState::HOOKED:
      this->start_scan_();
      break;
    case LRScanState::SET_PARAMS:
    case LRScanState::STARTING:
    case LRScanState::STOPPING:
      if ((int32_t) (now - this->state_since_ms_) > (int32_t) CONTROL_EVENT_TIMEOUT_MS)
        this->fail_("no completion event", -1);
      break;
    case LRScanState::RUNNING:
      // signed difference: last_report_ms_ may have been stamped after `now` was read (queue drain above)
      if (this->report_timeout_ms_ != 0 && (int32_t) (now - this->last_report_ms_) > (int32_t) this->report_timeout_ms_) {
        ESP_LOGW(TAG_SCAN, "No advertising report for %" PRIu32 " ms, restarting the extended scan",
                 now - this->last_report_ms_);
        this->scan_restarts_++;
        this->stop_scan_(true);
      }
      break;
    case LRScanState::FAILED:
      if ((int32_t) (now - this->retry_at_ms_) >= 0)
        this->start_scan_();
      break;
    default:
      break;
  }

  if (this->pending_count_ != 0)
    this->sweep_pending_(now, false);

#ifdef USE_BLE_LR_TRACKER
  if (this->tracker_ != nullptr)
    this->tracker_emulation_loop_(now);
#endif

  if (now - this->last_stats_ms_ >= this->stats_interval_ms_)
    this->log_stats_(now);
}

// ---------------------------------------------------------------------------------------------------------------
// GAP callback hook (BT task) and queue
// ---------------------------------------------------------------------------------------------------------------

void BLELongRangeScanner::gap_callback_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  auto *self = global_lr_scanner;
  switch (event) {
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT:
      self->enqueue_(LREvent::REPORT, 0, &param->ext_adv_report.params);
      return;
    case ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT:
      self->enqueue_(LREvent::SET_PARAMS_COMPLETE, param->set_ext_scan_params.status, nullptr);
      return;
    case ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT:
      self->enqueue_(LREvent::SCAN_START_COMPLETE, param->ext_scan_start.status, nullptr);
      return;
    case ESP_GAP_BLE_EXT_SCAN_STOP_COMPLETE_EVT:
      self->enqueue_(LREvent::SCAN_STOP_COMPLETE, param->ext_scan_stop.status, nullptr);
      return;
    case ESP_GAP_BLE_SCAN_TIMEOUT_EVT:
      self->enqueue_(LREvent::SCAN_TIMEOUT, 0, nullptr);
      return;
    default:
      break;
  }
  // Everything else belongs to esp32_ble (and through it to the tracker, clients, security...).
  if (self->prev_callback_ != nullptr)
    self->prev_callback_(event, param);
}

void BLELongRangeScanner::enqueue_(LREvent::Kind kind, uint8_t status, const esp_ble_gap_ext_adv_report_t *rep) {
  // BT task context: no logging, no allocation. The pool holds LR_QUEUE_SIZE-1 objects, the queue LR_QUEUE_SIZE-1
  // slots, so a successful allocate() guarantees a successful push() (single producer / single consumer).
  LREvent *ev = this->pool_.allocate();
  if (ev == nullptr) {
    this->queue_.increment_dropped_count();
    return;
  }
  ev->kind = kind;
  ev->status = status;
  if (rep != nullptr) {
    LRReport &r = ev->report;
    r.event_type = rep->event_type;
    r.addr_type = rep->addr_type;
    memcpy(r.addr, rep->addr, sizeof(r.addr));
    r.primary_phy = rep->primary_phy;
    r.secondary_phy = LR_SECONDARY_PHY(rep);
    r.sid = rep->sid;
    r.tx_power = (int8_t) rep->tx_power;
    r.rssi = rep->rssi;
    r.data_status = rep->data_status;
    uint8_t len = rep->adv_data_len;
    if (len > LR_MAX_ADV_DATA)
      len = LR_MAX_ADV_DATA;
    r.adv_data_len = len;
    if (len)
      memcpy(r.adv_data, rep->adv_data, len);
  }
  this->queue_.push(ev);
}

void BLELongRangeScanner::hook_() {
  esp_gap_ble_cb_t prev = esp_ble_gap_get_callback();
  if (prev != gap_callback_)
    this->prev_callback_ = prev;
  esp_err_t err = esp_ble_gap_register_callback(gap_callback_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %s", esp_err_to_name(err));
    this->retry_at_ms_ = millis() + 1000;
    return;
  }
  this->hooked_ = true;
  ESP_LOGD(TAG, "GAP callback hooked (chaining to esp32_ble handler %p)", (void *) this->prev_callback_);
  this->set_state_(LRScanState::HOOKED);
}

void BLELongRangeScanner::unhook_() {
  // The stack is going down (esp32_ble disable) – Bluedroid re-registers esp32_ble's callback on re-enable.
  ESP_LOGD(TAG, "BLE stack inactive, releasing the GAP hook");
  if (this->prev_callback_ != nullptr)
    esp_ble_gap_register_callback(this->prev_callback_);  // may fail if already disabled – harmless
  this->hooked_ = false;
  this->pending_count_ = 0;
  for (auto &p : this->pending_)
    p.used = false;
  for (auto &f : this->fragments_)
    f.used = false;
  this->set_state_(LRScanState::IDLE);
}

// ---------------------------------------------------------------------------------------------------------------
// Extended scan control
// ---------------------------------------------------------------------------------------------------------------

void BLELongRangeScanner::set_state_(LRScanState s) {
  if (s == this->state_)
    return;
  ESP_LOGD(TAG_SCAN, "state %s -> %s", lr_scan_state_to_string(this->state_), lr_scan_state_to_string(s));
  this->state_ = s;
  this->state_since_ms_ = millis();
  if (this->scanner_state_text_sensor_ != nullptr)
    this->scanner_state_text_sensor_->publish_state(lr_scan_state_to_string(s));
  if (this->scanning_binary_sensor_ != nullptr)
    this->scanning_binary_sensor_->publish_state(s == LRScanState::RUNNING);
  if (s == LRScanState::RUNNING)
    this->status_clear_warning();
}

void BLELongRangeScanner::start_scan_() {
  esp_ble_ext_scan_params_t params{};
  params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
  params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
  params.cfg_mask = 0;
  if (this->phy_mask_ & 0x01)
    params.cfg_mask |= ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
  if (this->phy_mask_ & 0x02)
    params.cfg_mask |= ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK;
  params.uncoded_cfg.scan_type = this->active_1m_ ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE;
  params.uncoded_cfg.scan_interval = this->interval_1m_;
  params.uncoded_cfg.scan_window = this->window_1m_;
  params.coded_cfg.scan_type = BLE_SCAN_TYPE_PASSIVE;  // pvvx LR advertising is not scannable, name is in AUX_ADV_IND
  params.coded_cfg.scan_interval = this->interval_coded_;
  params.coded_cfg.scan_window = this->window_coded_;

  ESP_LOGI(TAG_SCAN, "Starting extended scan (PHY mask 0x%02x): 1M %s %.1f/%.1f ms, Coded passive %.1f/%.1f ms, no duplicate filter",
           params.cfg_mask, this->active_1m_ ? "active" : "passive", this->interval_1m_ * 0.625f, this->window_1m_ * 0.625f,
           this->interval_coded_ * 0.625f, this->window_coded_ * 0.625f);
  this->restart_after_stop_ = false;
  esp_err_t err = esp_ble_gap_set_ext_scan_params(&params);
  if (err != ESP_OK) {
    this->fail_("esp_ble_gap_set_ext_scan_params", err);
    return;
  }
  this->set_state_(LRScanState::SET_PARAMS);
}

void BLELongRangeScanner::stop_scan_(bool restart) {
  if (this->state_ != LRScanState::RUNNING) {
    if (restart && (this->state_ == LRScanState::HOOKED || this->state_ == LRScanState::FAILED))
      this->start_scan_();
    return;
  }
  this->restart_after_stop_ = restart;
  esp_err_t err = esp_ble_gap_stop_ext_scan();
  if (err != ESP_OK) {
    this->fail_("esp_ble_gap_stop_ext_scan", err);
    return;
  }
  this->set_state_(LRScanState::STOPPING);
}

void BLELongRangeScanner::set_scan_ms(uint32_t interval_1m, uint32_t window_1m, bool active_1m,
                                      uint32_t interval_coded, uint32_t window_coded, uint8_t phy_mask) {
  auto units = [](uint32_t ms) -> uint16_t {
    uint32_t u = ms * 1000 / 625;
    if (u < 4)
      u = 4;
    if (u > 0x4000)
      u = 0x4000;
    return (uint16_t) u;
  };
  this->interval_1m_ = units(interval_1m);
  this->window_1m_ = units(std::min(window_1m, interval_1m));
  this->active_1m_ = active_1m;
  this->interval_coded_ = units(interval_coded);
  this->window_coded_ = units(std::min(window_coded, interval_coded));
  this->set_phy_mask(phy_mask & 0x03);
  ESP_LOGI(TAG_SCAN, "New scan parameters: PHY mask 0x%02x, 1M %s %" PRIu32 "/%" PRIu32 " ms, Coded %" PRIu32 "/%" PRIu32 " ms",
           this->phy_mask_, active_1m ? "active" : "passive", interval_1m, window_1m, interval_coded, window_coded);
  this->restart_scan();
}

void BLELongRangeScanner::set_coex_prefer_bt(bool prefer_bt) {
#ifdef USE_ESP32_BLE_SOFTWARE_COEXISTENCE
  this->coex_prefer_bt_ = prefer_bt;
  esp_err_t err = esp_coex_preference_set(prefer_bt ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE);
  ESP_LOGI(TAG, "Coexistence preference: %s (%s)", prefer_bt ? "Bluetooth" : "balanced", esp_err_to_name(err));
#else
  ESP_LOGW(TAG, "Software coexistence not compiled in – coexistence preference ignored");
#endif
}

void BLELongRangeScanner::restart_scan() {
  ESP_LOGI(TAG_SCAN, "Restart requested");
  this->scan_restarts_++;
  this->stop_scan_(true);
}

void BLELongRangeScanner::fail_(const char *what, int status) {
  this->scan_failures_++;
  ESP_LOGE(TAG_SCAN, "%s failed (status %d) in state %s – retry in %" PRIu32 " ms", what, status,
           lr_scan_state_to_string(this->state_), this->retry_backoff_ms_);
  this->status_set_warning();
  this->retry_at_ms_ = millis() + this->retry_backoff_ms_;
  this->retry_backoff_ms_ = std::min<uint32_t>(this->retry_backoff_ms_ * 2, MAX_BACKOFF_MS);
  this->set_state_(LRScanState::FAILED);
}

void BLELongRangeScanner::handle_event_(LREvent *ev) {
  const uint32_t now = millis();
  switch (ev->kind) {
    case LREvent::REPORT:
      this->handle_report_(ev->report);
      break;
    case LREvent::SET_PARAMS_COMPLETE:
      ESP_LOGD(TAG_SCAN, "SET_EXT_SCAN_PARAMS_COMPLETE status %u", ev->status);
      if (this->state_ != LRScanState::SET_PARAMS)
        break;
      if (ev->status != ESP_BT_STATUS_SUCCESS && ev->status != ESP_BT_STATUS_DONE) {
        this->fail_("set ext scan params", ev->status);
        break;
      }
      {
        esp_err_t err = esp_ble_gap_start_ext_scan(0 /* until stopped */, 0 /* no period */);
        if (err != ESP_OK) {
          this->fail_("esp_ble_gap_start_ext_scan", err);
          break;
        }
        this->set_state_(LRScanState::STARTING);
      }
      break;
    case LREvent::SCAN_START_COMPLETE:
      ESP_LOGD(TAG_SCAN, "EXT_SCAN_START_COMPLETE status %u", ev->status);
      if (ev->status != ESP_BT_STATUS_SUCCESS && ev->status != ESP_BT_STATUS_DONE) {
        this->fail_("start ext scan", ev->status);
        break;
      }
      ESP_LOGI(TAG_SCAN, "Extended scan running (1M + Coded PHY)");
      if (this->coex_prefer_bt_ && this->scan_started_ms_ == 0)
        this->set_coex_prefer_bt(true);
      this->retry_backoff_ms_ = 5000;
      this->scan_started_ms_ = now;
      this->last_report_ms_ = now;
      this->set_state_(LRScanState::RUNNING);
      break;
    case LREvent::SCAN_STOP_COMPLETE:
      ESP_LOGD(TAG_SCAN, "EXT_SCAN_STOP_COMPLETE status %u", ev->status);
      this->sweep_pending_(now, true);
      // loop() restarts the scan from HOOKED: the extended scan is meant to run continuously.
      this->set_state_(LRScanState::HOOKED);
      break;
    case LREvent::SCAN_TIMEOUT:
      ESP_LOGW(TAG_SCAN, "SCAN_TIMEOUT (duration expired) – restarting");
      this->sweep_pending_(now, true);
      this->set_state_(LRScanState::HOOKED);
      break;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Report processing (main loop)
// ---------------------------------------------------------------------------------------------------------------

LRDevice *BLELongRangeScanner::find_device_(uint64_t mac) {
  for (auto *d : this->devices_) {
    if (d->mac == mac)
      return d;
  }
  return nullptr;
}

void BLELongRangeScanner::handle_report_(LRReport &r) {
  const uint32_t now = millis();
  this->last_report_ms_ = now;
  this->total_reports_++;
  if (r.primary_phy == ESP_BLE_GAP_PHY_CODED) {
    this->reports_coded_++;
    if (r.rssi > this->best_coded_rssi_)
      this->best_coded_rssi_ = r.rssi;
    if (r.rssi < this->worst_coded_rssi_)
      this->worst_coded_rssi_ = r.rssi;
  } else {
    this->reports_1m_++;
  }
  if (r.secondary_phy == ESP_BLE_GAP_PHY_2M)
    this->reports_2m_++;
  if (r.is_legacy()) {
    this->reports_legacy_++;
  } else {
    this->reports_ext_++;
  }
  if (r.is_scan_rsp())
    this->reports_scan_rsp_++;

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  {
    char mac_s[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(r.addr, mac_s);
    char hex[LR_MAX_ADV_DATA * 3 + 1];
    format_hex_pretty_to(hex, sizeof(hex), r.adv_data, r.adv_data_len, '.');
    ESP_LOGVV(TAG_SCAN, "[%s] evt=0x%02x %s%s phy=%s/%s sid=%u rssi=%d tx=%d status=%u len=%u data=%s", mac_s,
              r.event_type, r.is_legacy() ? "legacy" : "ext", r.is_scan_rsp() ? " scan_rsp" : "",
              lr_phy_to_string(r.primary_phy), lr_phy_to_string(r.secondary_phy), r.sid, r.rssi, r.tx_power,
              r.data_status, r.adv_data_len, hex);
  }
#endif

  // --- fragment reassembly (AUX_CHAIN_IND: data_status = incomplete, more to come) ---
  if (r.data_status == DATA_STATUS_TRUNCATED) {
    this->fragments_truncated_++;
    for (auto &f : this->fragments_) {
      if (f.used && f.sid == r.sid && memcmp(f.addr, r.addr, 6) == 0)
        f.used = false;
    }
    return;
  }
  if (r.data_status == DATA_STATUS_INCOMPLETE || r.data_status == DATA_STATUS_COMPLETE) {
    Fragment *slot = nullptr;
    Fragment *free_slot = nullptr;
    for (auto &f : this->fragments_) {
      if (f.used && f.sid == r.sid && memcmp(f.addr, r.addr, 6) == 0)
        slot = &f;
      else if (!f.used || now - f.stored_ms > 2000)
        free_slot = &f;
    }
    if (r.data_status == DATA_STATUS_INCOMPLETE) {
      this->fragments_incomplete_++;
      if (slot == nullptr) {
        slot = free_slot;
        if (slot == nullptr)
          return;  // no room – drop this chain
        slot->used = true;
        memcpy(slot->addr, r.addr, 6);
        slot->sid = r.sid;
        slot->len = 0;
      }
      slot->stored_ms = now;
      uint16_t room = LR_MAX_ADV_DATA - slot->len;
      uint16_t n = r.adv_data_len < room ? r.adv_data_len : room;
      memcpy(slot->data + slot->len, r.adv_data, n);
      slot->len += n;
      return;
    }
    if (slot != nullptr) {  // COMPLETE after fragments: prepend the stored part
      uint16_t total = slot->len + r.adv_data_len;
      if (total > LR_MAX_ADV_DATA)
        total = LR_MAX_ADV_DATA;
      uint8_t buf[LR_MAX_ADV_DATA];
      memcpy(buf, slot->data, slot->len);
      memcpy(buf + slot->len, r.adv_data, total - slot->len);
      memcpy(r.adv_data, buf, total);
      r.adv_data_len = (uint8_t) total;
      slot->used = false;
    }
  }

  // --- legacy ADV + SCAN_RSP merge (Bluedroid's BLE 5.0 report path delivers them separately) ---
  if (r.is_legacy()) {
    if (r.is_scan_rsp()) {
      for (auto &p : this->pending_) {
        if (p.used && memcmp(p.report.addr, r.addr, 6) == 0) {
          p.used = false;
          this->pending_count_--;
          this->deliver_(p.report, p.report.adv_data, p.report.adv_data_len, r.adv_data, r.adv_data_len);
          return;
        }
      }
      this->deliver_(r, nullptr, 0, r.adv_data, r.adv_data_len);  // unmatched scan response
      return;
    }
    if (r.is_scannable() && this->active_1m_) {
      PendingAdv *slot = nullptr;
      for (auto &p : this->pending_) {
        if (p.used && memcmp(p.report.addr, r.addr, 6) == 0) {
          // re-advertisement before the scan response: deliver the held one, reuse the slot
          this->deliver_(p.report, p.report.adv_data, p.report.adv_data_len, nullptr, 0);
          slot = &p;
          break;
        }
      }
      if (slot == nullptr) {
        for (auto &p : this->pending_) {
          if (!p.used) {
            slot = &p;
            this->pending_count_++;
            break;
          }
        }
      }
      if (slot != nullptr) {
        slot->used = true;
        slot->stored_ms = now;
        slot->report = r;
        return;
      }
      // table full: deliver unmerged
    }
  }
  this->deliver_(r, r.adv_data, r.adv_data_len, nullptr, 0);
}

void BLELongRangeScanner::sweep_pending_(uint32_t now, bool flush_all) {
  for (auto &p : this->pending_) {
    if (!p.used)
      continue;
    if (flush_all || now - p.stored_ms > PENDING_TIMEOUT_MS) {
      p.used = false;
      this->pending_count_--;
      this->deliver_(p.report, p.report.adv_data, p.report.adv_data_len, nullptr, 0);
    }
  }
}

void BLELongRangeScanner::deliver_(const LRReport &r, const uint8_t *adv, uint8_t adv_len, const uint8_t *rsp,
                                   uint8_t rsp_len) {
  uint8_t buf[LR_MAX_ADV_DATA + 31];
  size_t total = 0;
  if (adv != nullptr && adv_len) {
    memcpy(buf, adv, adv_len);
    total += adv_len;
  }
  if (rsp != nullptr && rsp_len) {
    size_t n = rsp_len;
    if (total + n > sizeof(buf))
      n = sizeof(buf) - total;
    memcpy(buf + total, rsp, n);
    total += n;
    rsp_len = (uint8_t) n;
  }

  AdvFields fields{};
  parse_ad_structures(buf, total, fields);

  LRDevice *dev = this->find_device_(r.address_uint64());
  if (dev != nullptr) {
    this->publish_device_(dev, r, fields);
  } else if (this->log_unknown_ && fields.meas.valid) {
    char mac_s[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(r.addr, mac_s);
    ESP_LOGD(TAG_DEV, "[%s] unconfigured %s phy=%s rssi=%d name='%s' T=%.2f H=%.2f B=%u%% %u mV", mac_s,
             adv_format_to_string(fields.meas.format), lr_phy_to_string(r.primary_phy), r.rssi, fields.name,
             fields.meas.temperature, fields.meas.humidity, fields.meas.battery_pct, fields.meas.battery_mv);
  }

#ifdef USE_BLE_LR_TRACKER
  if (this->tracker_ != nullptr && this->forward_to_tracker_) {
    if (total <= sizeof(esp32_ble::BLEScanResult::ble_adv)) {
      esp32_ble::BLEScanResult sr{};
      memcpy(sr.bda, r.addr, sizeof(sr.bda));
      sr.ble_addr_type = r.addr_type;
      sr.rssi = r.rssi;
      memcpy(sr.ble_adv, buf, total);
      sr.adv_data_len = (uint8_t) (total - rsp_len);
      sr.scan_rsp_len = rsp_len;
      sr.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
      this->tracker_->gap_scan_event_handler(sr);
      this->forwarded_++;
    } else {
      this->not_forwarded_too_long_++;
    }
  }
#endif

  for (auto &cb : this->report_callbacks_)
    cb(r);
}

void BLELongRangeScanner::publish_device_(LRDevice *dev, const LRReport &r, const AdvFields &f) {
  const uint32_t now = millis();
  if (dev->first_seen_ms == 0)
    dev->first_seen_ms = now;
  dev->last_seen_ms = now;
  if (r.is_coded()) {
    dev->reports_coded++;
  } else {
    dev->reports_1m++;
  }
  const AdvMeasurement &m = f.meas;

  char mac_s[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(r.addr, mac_s);
  ESP_LOGD(TAG_DEV, "[%s] %s: phy=%s/%s %s rssi=%d fmt=%s%s T=%.2f°C H=%.2f%% batt=%u%% %u mV cnt=%u name='%s' len=%u",
           mac_s, dev->name, lr_phy_to_string(r.primary_phy), lr_phy_to_string(r.secondary_phy),
           r.is_legacy() ? "legacy" : "ext", r.rssi, adv_format_to_string(m.format), m.encrypted ? " (encrypted)" : "",
           m.has_temperature ? m.temperature : NAN, m.has_humidity ? m.humidity : NAN, m.battery_pct, m.battery_mv,
           m.counter, f.name, r.adv_data_len);

  if (dev->rssi != nullptr)
    dev->rssi->publish_state(r.rssi);
  if (dev->phy != nullptr && r.primary_phy != dev->last_primary_phy)
    dev->phy->publish_state(lr_phy_to_string(r.primary_phy));
  if (dev->long_range != nullptr && (dev->last_primary_phy == 0 || r.is_coded() != (dev->last_primary_phy == ESP_BLE_GAP_PHY_CODED)))
    dev->long_range->publish_state(r.is_coded());
  dev->last_primary_phy = r.primary_phy;

  if (!m.valid || m.encrypted)
    return;
  if (dev->format != nullptr && m.format != dev->last_format) {
    dev->format->publish_state(adv_format_to_string(m.format));
    dev->last_format = m.format;
  }
  if (dev->temperature != nullptr && m.has_temperature)
    dev->temperature->publish_state(m.temperature);
  if (dev->humidity != nullptr && m.has_humidity)
    dev->humidity->publish_state(m.humidity);
  if (dev->battery != nullptr && m.has_battery_pct)
    dev->battery->publish_state(m.battery_pct);
  if (dev->battery_voltage != nullptr && m.has_battery_mv)
    dev->battery_voltage->publish_state(m.battery_mv / 1000.0f);
  if (dev->packet_counter != nullptr && m.has_counter)
    dev->packet_counter->publish_state(m.counter);
}

// ---------------------------------------------------------------------------------------------------------------
// esp32_ble_tracker legacy-scan emulation (link-time wrappers, see header)
// ---------------------------------------------------------------------------------------------------------------

#ifdef USE_BLE_LR_TRACKER
void BLELongRangeScanner::tracker_emulate_set_params(const esp_ble_scan_params_t *params) {
  if (params != nullptr)
    this->tracker_wants_active_ = params->scan_type == BLE_SCAN_TYPE_ACTIVE;
  this->tracker_pending_params_ = true;
}

void BLELongRangeScanner::tracker_emulate_start(uint32_t duration_s) {
  this->tracker_duration_ms_ = duration_s * 1000;
  this->tracker_pending_start_ = true;
}

void BLELongRangeScanner::tracker_emulate_stop() { this->tracker_pending_stop_ = true; }

void BLELongRangeScanner::tracker_emulation_loop_(uint32_t now) {
  // Deliver the synthetic completion events from the main loop (never re-entrantly from inside the tracker).
  if (this->tracker_pending_params_) {
    this->tracker_pending_params_ = false;
    esp_ble_gap_cb_param_t p{};
    p.scan_param_cmpl.status = ESP_BT_STATUS_SUCCESS;
    this->tracker_->gap_event_handler(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    // The tracker's active/passive choice (YAML or Home Assistant via bluetooth_proxy) drives the 1M part of the
    // extended scan.
    if (this->tracker_wants_active_ != this->active_1m_) {
      ESP_LOGI(TAG_SCAN, "Tracker requests %s scanning – applying to the 1M PHY",
               this->tracker_wants_active_ ? "active" : "passive");
      this->active_1m_ = this->tracker_wants_active_;
      this->restart_scan();
    }
  }
  if (this->tracker_pending_start_) {
    this->tracker_pending_start_ = false;
    esp_ble_gap_cb_param_t p{};
    p.scan_start_cmpl.status = ESP_BT_STATUS_SUCCESS;
    this->tracker_->gap_event_handler(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    this->tracker_emulated_running_ = true;
    this->tracker_scan_end_ms_ = now + this->tracker_duration_ms_;
    ESP_LOGD(TAG_SCAN, "Tracker scan emulated: period %" PRIu32 " s (extended scan keeps running)",
             this->tracker_duration_ms_ / 1000);
  }
  if (this->tracker_pending_stop_) {
    this->tracker_pending_stop_ = false;
    this->tracker_emulated_running_ = false;
    esp_ble_gap_cb_param_t p{};
    p.scan_stop_cmpl.status = ESP_BT_STATUS_SUCCESS;
    this->tracker_->gap_event_handler(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
  }
  if (this->tracker_emulated_running_ && this->tracker_duration_ms_ != 0 &&
      (int32_t) (now - this->tracker_scan_end_ms_) >= 0) {
    // End of the tracker's scan period: it cleans up, fires on_scan_end and (continuous) starts again.
    this->tracker_emulated_running_ = false;
    esp32_ble::BLEScanResult sr{};
    sr.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
    this->tracker_->gap_scan_event_handler(sr);
  }
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Statistics / config dump
// ---------------------------------------------------------------------------------------------------------------

void BLELongRangeScanner::log_stats_(uint32_t now) {
  const uint32_t dt = now - this->last_stats_ms_;
  this->last_stats_ms_ = now;
  const float per_min_1m = dt ? (this->reports_1m_ - this->stats_1m_prev_) * 60000.0f / dt : 0;
  const float per_min_coded = dt ? (this->reports_coded_ - this->stats_coded_prev_) * 60000.0f / dt : 0;
  this->stats_1m_prev_ = this->reports_1m_;
  this->stats_coded_prev_ = this->reports_coded_;
  const uint32_t free_heap = esp_get_free_heap_size();
  const uint32_t min_heap = esp_get_minimum_free_heap_size();
  if (free_heap < this->min_free_heap_)
    this->min_free_heap_ = free_heap;

  ESP_LOGI(TAG,
           "stats: state=%s scan_uptime=%" PRIu32 "s total=%" PRIu32 " 1M=%" PRIu32 " (%.1f/min) coded=%" PRIu32
           " (%.1f/min) 2M-sec=%" PRIu32 " legacy=%" PRIu32 " ext=%" PRIu32 " scan_rsp=%" PRIu32 " frag=%" PRIu32
           "/%" PRIu32 " fwd=%" PRIu32 " too_long=%" PRIu32 " dropped=%" PRIu32 " restarts=%" PRIu32
           " failures=%" PRIu32 " heap=%" PRIu32 " min_heap=%" PRIu32 " coded_rssi=[%d..%d]",
           lr_scan_state_to_string(this->state_),
           this->state_ == LRScanState::RUNNING ? (now - this->scan_started_ms_) / 1000 : 0, this->total_reports_,
           this->reports_1m_, per_min_1m, this->reports_coded_, per_min_coded, this->reports_2m_, this->reports_legacy_,
           this->reports_ext_, this->reports_scan_rsp_, this->fragments_incomplete_, this->fragments_truncated_,
           this->forwarded_, this->not_forwarded_too_long_, this->dropped_, this->scan_restarts_, this->scan_failures_,
           free_heap, min_heap, this->worst_coded_rssi_, this->best_coded_rssi_);
  for (auto *d : this->devices_) {
    ESP_LOGI(TAG, "  %s: 1M=%" PRIu32 " coded=%" PRIu32 " last_seen=%s phy=%s fmt=%s", d->name, d->reports_1m,
             d->reports_coded, d->last_seen_ms ? (std::to_string((now - d->last_seen_ms) / 1000) + "s ago").c_str() : "never",
             lr_phy_to_string(d->last_primary_phy), adv_format_to_string(d->last_format));
  }
  if (this->reports_1m_sensor_ != nullptr)
    this->reports_1m_sensor_->publish_state(per_min_1m);
  if (this->reports_coded_sensor_ != nullptr)
    this->reports_coded_sensor_->publish_state(per_min_coded);
  if (this->free_heap_sensor_ != nullptr)
    this->free_heap_sensor_->publish_state(free_heap);
}

void BLELongRangeScanner::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Long Range scanner (extended scan, BLE 5.0):");
  ESP_LOGCONFIG(TAG,
                "  PHYs: %s%s\n"
                "  1M PHY: %s, interval %.1f ms, window %.1f ms\n"
                "  Coded PHY: passive, interval %.1f ms, window %.1f ms\n"
                "  Report timeout: %" PRIu32 " ms, stats every %" PRIu32 " ms\n"
                "  Queue: %u reports x %u B\n"
                "  State: %s",
                (this->phy_mask_ & 0x01) ? "1M " : "", (this->phy_mask_ & 0x02) ? "Coded" : "",
                this->active_1m_ ? "active" : "passive", this->interval_1m_ * 0.625f, this->window_1m_ * 0.625f,
                this->interval_coded_ * 0.625f, this->window_coded_ * 0.625f, this->report_timeout_ms_,
                this->stats_interval_ms_, LR_QUEUE_SIZE - 1, (unsigned) sizeof(LREvent),
                lr_scan_state_to_string(this->state_));
#ifdef USE_BLE_LR_TRACKER
  ESP_LOGCONFIG(TAG, "  esp32_ble_tracker: legacy scan emulated, reports forwarded: %s", YESNO(this->tracker_ != nullptr && this->forward_to_tracker_));
#else
  ESP_LOGCONFIG(TAG, "  esp32_ble_tracker: not configured");
#endif
  for (auto *d : this->devices_) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++)
      mac[i] = (uint8_t) (d->mac >> (8 * (5 - i)));
    char mac_s[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(mac, mac_s);
    ESP_LOGCONFIG(TAG, "  Device %s: %s", mac_s, d->name);
  }
}

}  // namespace esphome::ble_longrange_scanner

#ifdef USE_BLE_LR_TRACKER
// Link-time wrappers (-Wl,--wrap=esp_ble_gap_set_scan_params,...) for the tracker's legacy scan API.
// Only esp32_ble_tracker calls these in an ESPHome build; the real functions stay reachable as __real_*.
extern "C" {
esp_err_t __real_esp_ble_gap_set_scan_params(esp_ble_scan_params_t *scan_params);
esp_err_t __real_esp_ble_gap_start_scanning(uint32_t duration);
esp_err_t __real_esp_ble_gap_stop_scanning(void);

esp_err_t __wrap_esp_ble_gap_set_scan_params(esp_ble_scan_params_t *scan_params) {
  auto *s = esphome::ble_longrange_scanner::global_lr_scanner;
  if (s == nullptr)
    return __real_esp_ble_gap_set_scan_params(scan_params);
  s->tracker_emulate_set_params(scan_params);
  return ESP_OK;
}
esp_err_t __wrap_esp_ble_gap_start_scanning(uint32_t duration) {
  auto *s = esphome::ble_longrange_scanner::global_lr_scanner;
  if (s == nullptr)
    return __real_esp_ble_gap_start_scanning(duration);
  s->tracker_emulate_start(duration);
  return ESP_OK;
}
esp_err_t __wrap_esp_ble_gap_stop_scanning(void) {
  auto *s = esphome::ble_longrange_scanner::global_lr_scanner;
  if (s == nullptr)
    return __real_esp_ble_gap_stop_scanning();
  s->tracker_emulate_stop();
  return ESP_OK;
}
}
#endif  // USE_BLE_LR_TRACKER

#endif  // USE_ESP32
