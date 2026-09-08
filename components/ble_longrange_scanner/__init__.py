"""ESPHome external component: ble_longrange_scanner.

BLE 5.0 extended scanning (LE 1M + LE Coded PHY, "Long Range") on ESP32-C3/S3/C6 with Bluedroid, so that
Xiaomi/Telink thermometers running pvvx ATC_MiThermometer with "BT5 PHY + LE Long Range" enabled are seen and
decoded (BTHome v2 / pvvx / atc1441 / Mi) into Home Assistant entities. Legacy (BT 4.2) advertisements are
received by the same scan and can be forwarded to esp32_ble_tracker (sensor platforms, bluetooth_proxy).
"""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, esp32, esp32_ble, sensor, text_sensor
from esphome.const import (
    CONF_ACTIVE,
    CONF_BATTERY_VOLTAGE,
    CONF_CONTINUOUS,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MAC_ADDRESS,
    CONF_NAME,
    CONF_TEMPERATURE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_VOLT,
)
from esphome.components.esp32 import get_esp32_variant
from esphome.components.esp32.const import (
    VARIANT_ESP32C2,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    VARIANT_ESP32S3,
)
from esphome.core import CORE, TimePeriod

# BLE 5.0 (extended advertising / Coded PHY) is a controller feature: the original ESP32 and ESP32-S2 do not have it
# (IDF: SOC_BLE_50_SUPPORTED). The classic ESP32 can only ever see legacy (BT 4.2) advertisements.
BLE50_VARIANTS = {VARIANT_ESP32C2, VARIANT_ESP32C3, VARIANT_ESP32C5, VARIANT_ESP32C6, VARIANT_ESP32H2, VARIANT_ESP32S3}


def _require_ble50(config):
    variant = get_esp32_variant()
    if variant not in BLE50_VARIANTS:
        raise cv.Invalid(
            f"ble_longrange_scanner needs a BLE 5.0 controller (Coded PHY / extended scanning); the {variant} "
            "controller is BLE 4.2 only and cannot receive Long Range advertisements. Use ESP32-C3/S3/C6/C5/H2."
        )
    return config

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["esp32", "esp32_ble"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]
CODEOWNERS = ["@inteltryb93"]

ns = cg.esphome_ns.namespace("ble_longrange_scanner")
BLELongRangeScanner = ns.class_(
    "BLELongRangeScanner", cg.Component, cg.Parented.template(esp32_ble.ESP32BLE)
)
LRDevice = ns.struct("LRDevice")

CONF_TRACKER_ID = "esp32_ble_tracker_id"
CONF_SCAN_PARAMETERS = "scan_parameters"
CONF_WINDOW = "window"
CONF_CODED_INTERVAL = "coded_interval"
CONF_CODED_WINDOW = "coded_window"
CONF_SCAN_PHY = "phy"
PHY_MASKS = {"both": 3, "1m": 1, "coded": 2}
CONF_REPORT_TIMEOUT = "report_timeout"
CONF_STATS_INTERVAL = "stats_interval"
CONF_FORWARD_TO_TRACKER = "forward_to_tracker"
CONF_LOG_UNKNOWN = "log_unknown_devices"
CONF_COEX_PREFER_BT = "coex_prefer_bt"
CONF_DEVICES = "devices"
CONF_BATTERY = "battery"
CONF_RSSI = "rssi"
CONF_PACKET_COUNTER = "packet_counter"
CONF_PHY = "phy"
CONF_FORMAT = "format"
CONF_LONG_RANGE = "long_range"
CONF_REPORTS_1M = "reports_1m"
CONF_REPORTS_CODED = "reports_coded"
CONF_FREE_HEAP = "free_heap"
CONF_SCANNER_STATE = "scanner_state"
CONF_SCANNING = "scanning"

# Per-device entities: (key, default name suffix, schema, setter)
_DEVICE_SENSORS = {
    CONF_TEMPERATURE: (
        "Temperature",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_HUMIDITY: (
        "Humidity",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_BATTERY: (
        "Battery",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    CONF_BATTERY_VOLTAGE: (
        "Battery Voltage",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    CONF_RSSI: (
        "RSSI",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    CONF_PACKET_COUNTER: (
        "Packet Counter",
        sensor.sensor_schema(
            accuracy_decimals=0,
            icon="mdi:counter",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
}
_DEVICE_TEXT_SENSORS = {
    CONF_PHY: (
        "PHY",
        text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:radio-tower"
        ),
    ),
    CONF_FORMAT: (
        "Advertising Format",
        text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:code-json"
        ),
    ),
}
_DEVICE_BINARY_SENSORS = {
    CONF_LONG_RANGE: (
        "Long Range",
        binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:signal-distance-variant"
        ),
    ),
}


def _entity_or_false(schema):
    """Entity config, or `false` to skip creating the entity."""
    return cv.Any(cv.boolean_false, schema)


def _fill_device_defaults(config):
    """Create every per-device entity by default, named "<device name> <suffix>" (runs before the schema)."""
    if not isinstance(config, dict) or CONF_NAME not in config:
        return config
    config = dict(config)
    for table in (_DEVICE_SENSORS, _DEVICE_TEXT_SENSORS, _DEVICE_BINARY_SENSORS):
        for key, (suffix, _schema) in table.items():
            if key not in config:
                config[key] = {CONF_NAME: f"{config[CONF_NAME]} {suffix}"}
    return config


DEVICE_SCHEMA = cv.All(
    _fill_device_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LRDevice),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Required(CONF_NAME): cv.string_strict,
            **{
                cv.Optional(k): _entity_or_false(v[1])
                for k, v in {
                    **_DEVICE_SENSORS,
                    **_DEVICE_TEXT_SENSORS,
                    **_DEVICE_BINARY_SENSORS,
                }.items()
            },
        }
    ),
)


def _ble_units(value: TimePeriod) -> int:
    return value.total_microseconds // 625


def _validate_scan_parameters(config):
    for name_i, name_w in (
        (CONF_INTERVAL, CONF_WINDOW),
        (CONF_CODED_INTERVAL, CONF_CODED_WINDOW),
    ):
        interval, window = config[name_i], config[name_w]
        for n, v in ((name_i, interval), (name_w, window)):
            if v.total_microseconds < 2500 or v.total_microseconds > 10_240_000:
                raise cv.Invalid(f"{n} ({v}) must be between 2.5 ms and 10240 ms")
        if window > interval:
            raise cv.Invalid(f"{name_w} ({window}) must not exceed {name_i} ({interval})")
    # One radio serves both PHYs: warn (not fail) when the windows cannot both fit into a common interval,
    # the controller then time-shares them (measured on ESP32-C3, see docs/test_report.md).
    if config[CONF_INTERVAL] == config[CONF_CODED_INTERVAL] and (
        config[CONF_WINDOW].total_microseconds + config[CONF_CODED_WINDOW].total_microseconds
        > config[CONF_INTERVAL].total_microseconds
    ):
        _LOGGER.warning(
            "ble_longrange_scanner: %s + %s exceed %s, the controller will time-share the PHYs",
            CONF_WINDOW,
            CONF_CODED_WINDOW,
            CONF_INTERVAL,
        )
    return config


SCAN_PARAMETERS_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_INTERVAL, default="400ms"): cv.positive_time_period,
            cv.Optional(CONF_WINDOW, default="80ms"): cv.positive_time_period,
            cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
            cv.Optional(CONF_CODED_INTERVAL, default="400ms"): cv.positive_time_period,
            cv.Optional(CONF_CODED_WINDOW, default="300ms"): cv.positive_time_period,
            cv.Optional(CONF_SCAN_PHY, default="both"): cv.one_of(*PHY_MASKS, lower=True),
        }
    ),
    _validate_scan_parameters,
)


def _auto_tracker(config):
    """Bind to esp32_ble_tracker automatically when it is configured (forwarding of all reports)."""
    if CONF_TRACKER_ID not in config and "esp32_ble_tracker" in CORE.loaded_integrations:
        from esphome.components import esp32_ble_tracker

        config = dict(config)
        config[CONF_TRACKER_ID] = cv.use_id(esp32_ble_tracker.ESP32BLETracker)(None)
    return config


def _tracker_id_validator(value):
    from esphome.components import esp32_ble_tracker

    return cv.use_id(esp32_ble_tracker.ESP32BLETracker)(value)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLELongRangeScanner),
            cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
            cv.Optional(CONF_TRACKER_ID): _tracker_id_validator,
            cv.Optional(CONF_SCAN_PARAMETERS, default={}): SCAN_PARAMETERS_SCHEMA,
            cv.Optional(CONF_REPORT_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STATS_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FORWARD_TO_TRACKER, default=True): cv.boolean,
            cv.Optional(CONF_LOG_UNKNOWN, default=False): cv.boolean,
            cv.Optional(CONF_COEX_PREFER_BT, default=True): cv.boolean,
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(DEVICE_SCHEMA),
            cv.Optional(CONF_REPORTS_1M): sensor.sensor_schema(
                unit_of_measurement="reports/min",
                accuracy_decimals=1,
                icon="mdi:bluetooth",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_REPORTS_CODED): sensor.sensor_schema(
                unit_of_measurement="reports/min",
                accuracy_decimals=1,
                icon="mdi:signal-distance-variant",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FREE_HEAP): sensor.sensor_schema(
                unit_of_measurement="B",
                accuracy_decimals=0,
                icon="mdi:memory",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SCANNER_STATE): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:state-machine"
            ),
            cv.Optional(CONF_SCANNING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_RUNNING, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_esp_idf if hasattr(cv, "only_with_esp_idf") else (lambda c: c),
    _require_ble50,
    _auto_tracker,
)




async def to_code(config):
    # Bluedroid BLE 5.0 API (extended scan). ESPHome's reconcile step defaults 5.0 to off; values set here win.
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_50_FEATURES_SUPPORTED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_50_EXTEND_SCAN_EN", True)
    # Keep the 4.2 API compiled as well: esp32_ble_tracker / esp32_ble_client still reference it.
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_42_FEATURES_SUPPORTED", True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    cg.add(var.set_parent(parent))

    params = config[CONF_SCAN_PARAMETERS]
    cg.add(
        var.set_scan_1m(
            _ble_units(params[CONF_INTERVAL]),
            _ble_units(params[CONF_WINDOW]),
            params[CONF_ACTIVE],
        )
    )
    cg.add(
        var.set_scan_coded(
            _ble_units(params[CONF_CODED_INTERVAL]), _ble_units(params[CONF_CODED_WINDOW])
        )
    )
    cg.add(var.set_phy_mask(PHY_MASKS[params[CONF_SCAN_PHY]]))
    cg.add(var.set_report_timeout(config[CONF_REPORT_TIMEOUT]))
    cg.add(var.set_stats_interval(config[CONF_STATS_INTERVAL]))
    cg.add(var.set_forward_to_tracker(config[CONF_FORWARD_TO_TRACKER]))
    cg.add(var.set_log_unknown(config[CONF_LOG_UNKNOWN]))
    cg.add(var.set_coex_prefer_bt_on_boot(config[CONF_COEX_PREFER_BT]))

    if CONF_TRACKER_ID in config:
        cg.add_define("USE_BLE_LR_TRACKER")
        tracker = await cg.get_variable(config[CONF_TRACKER_ID])
        cg.add(var.set_tracker(tracker))
        # The tracker's legacy scan cannot run next to the extended scan (the controller refuses one while the
        # other is active). Redirect its three scan API calls to the component, which emulates the completion
        # events, so esp32_ble_tracker / bluetooth_proxy behave exactly as on a regular Bluetooth proxy.
        for fn in ("esp_ble_gap_set_scan_params", "esp_ble_gap_start_scanning", "esp_ble_gap_stop_scanning"):
            cg.add_build_flag(f"-Wl,--wrap={fn}")

    for dev in config[CONF_DEVICES]:
        dev_var = cg.Pvariable(
            dev[CONF_ID], var.add_device(dev[CONF_MAC_ADDRESS].as_hex, dev[CONF_NAME])
        )
        for key in _DEVICE_SENSORS:
            if dev.get(key):
                cg.add(getattr(dev_var, f"set_{key}")(await sensor.new_sensor(dev[key])))
        for key in _DEVICE_TEXT_SENSORS:
            if dev.get(key):
                cg.add(getattr(dev_var, f"set_{key}")(await text_sensor.new_text_sensor(dev[key])))
        for key in _DEVICE_BINARY_SENSORS:
            if dev.get(key):
                cg.add(getattr(dev_var, f"set_{key}")(await binary_sensor.new_binary_sensor(dev[key])))

    if CONF_REPORTS_1M in config:
        cg.add(var.set_reports_1m_sensor(await sensor.new_sensor(config[CONF_REPORTS_1M])))
    if CONF_REPORTS_CODED in config:
        cg.add(var.set_reports_coded_sensor(await sensor.new_sensor(config[CONF_REPORTS_CODED])))
    if CONF_FREE_HEAP in config:
        cg.add(var.set_free_heap_sensor(await sensor.new_sensor(config[CONF_FREE_HEAP])))
    if CONF_SCANNER_STATE in config:
        cg.add(var.set_scanner_state_text_sensor(await text_sensor.new_text_sensor(config[CONF_SCANNER_STATE])))
    if CONF_SCANNING in config:
        cg.add(var.set_scanning_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_SCANNING])))
