/*
 * Matter front-end.
 *
 *   EP 0            root node (Ethernet network commissioning)
 *   EP keys         Generic Switch, momentary (MS | MSR | MSL | MSM)
 *   EP outputs      Dimmable Light
 *   EP indicators   On/Off Light ("Key n LED")
 *   EP sensors      Temperature Sensor, Humidity Sensor (if SHT4x present)
 *
 * Threading: attribute callbacks run on the Matter task and only call dm_*
 * setters. Device-model events arrive on the DM task and are marshalled to
 * the Matter task with ScheduleLambda(). s_applying suppresses the echo of
 * our own attribute::update() calls; the device model additionally drops
 * no-op changes, so there is no update loop with MQTT either.
 */
#include "matter_app.h"
#include "app_settings.h"
#include "device_model.h"
#include "sensors.h"
#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_system.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "matter";

static uint16_t s_key_ep[DM_MAX_KEYS];
static uint16_t s_out_ep[DM_MAX_OUTPUTS];
static uint16_t s_ind_ep[DM_MAX_INDICATORS];
static uint16_t s_temp_ep, s_hum_ep;
static bool s_started;
static bool s_applying;   /* Matter task only */

static int find_ep(const uint16_t *arr, uint8_t n, uint16_t ep)
{
    for (uint8_t i = 0; i < n; i++) {
        if (arr[i] == ep) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ Matter -> DM */

static esp_err_t attr_cb(callback_type_t type, uint16_t ep, uint32_t cluster_id, uint32_t attr_id,
                         esp_matter_attr_val_t *val, void *priv)
{
    if (type != POST_UPDATE || s_applying) {
        return ESP_OK;
    }
    int o = find_ep(s_out_ep, dm_output_count(), ep);
    int ind = find_ep(s_ind_ep, dm_indicator_count(), ep);

    if (cluster_id == OnOff::Id && attr_id == OnOff::Attributes::OnOff::Id) {
        if (o >= 0) {
            dm_output_set_on(o, val->val.b, DM_SRC_MATTER);
        } else if (ind >= 0) {
            dm_indicator_set(ind, val->val.b, DM_SRC_MATTER);
        }
    } else if (cluster_id == LevelControl::Id && attr_id == LevelControl::Attributes::CurrentLevel::Id && o >= 0) {
        if (val->val.u8 != 0xFF) {   /* 0xFF = null */
            dm_output_set_level(o, val->val.u8, DM_SRC_MATTER);
        }
    }
    return ESP_OK;
}

static esp_err_t identify_cb(identification::callback_type_t type, uint16_t ep, uint8_t effect_id,
                             uint8_t effect_variant, void *priv)
{
    /* Blink the key LED of the identified endpoint so the installer finds it. */
    int k = find_ep(s_key_ep, dm_key_count(), ep);
    int o = find_ep(s_out_ep, dm_output_count(), ep);
    int led = k >= 0 ? k : o;

    ESP_LOGI(TAG, "identify ep %u type %d", ep, (int)type);
    if (led >= 0 && led < dm_indicator_count()) {
        dm_indicator_set(led, type == identification::START, DM_SRC_MATTER);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ DM -> Matter */

static void push_output(uint8_t i, dm_output_state_t st)
{
    esp_matter_attr_val_t on = esp_matter_bool(st.on);
    esp_matter_attr_val_t lvl = esp_matter_nullable_uint8(nullable<uint8_t>(st.level));

    s_applying = true;
    attribute::update(s_out_ep[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
    attribute::update(s_out_ep[i], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &lvl);
    s_applying = false;
}

static void push_indicator(uint8_t i, bool on_)
{
    esp_matter_attr_val_t on = esp_matter_bool(on_);

    s_applying = true;
    attribute::update(s_ind_ep[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
    s_applying = false;
}

static void set_position(uint16_t ep, uint8_t pos)
{
    esp_matter_attr_val_t v = esp_matter_uint8(pos);
    attribute::update(ep, Switch::Id, Switch::Attributes::CurrentPosition::Id, &v);
}

static void push_key(uint16_t ep, dm_key_evt_t evt, uint8_t count)
{
    namespace sw = cluster::switch_cluster::event;

    switch (evt) {
    case DM_KEY_INITIAL_PRESS:
        set_position(ep, 1);
        sw::send_initial_press(ep, 1);
        break;
    case DM_KEY_MULTI_PRESS_ONGOING:
        set_position(ep, 1);
        sw::send_multi_press_ongoing(ep, 1, count);
        break;
    case DM_KEY_SHORT_RELEASE:
        set_position(ep, 0);
        sw::send_short_release(ep, 1);
        break;
    case DM_KEY_LONG_PRESS:
        sw::send_long_press(ep, 1);
        break;
    case DM_KEY_LONG_RELEASE:
        set_position(ep, 0);
        sw::send_long_release(ep, 1);
        break;
    case DM_KEY_MULTI_PRESS_COMPLETE:
        sw::send_multi_press_complete(ep, 1, count);
        break;
    }
}

static void push_sensor(dm_sensor_t s, int32_t v)
{
    if (s == DM_SENSOR_TEMPERATURE && s_temp_ep) {
        esp_matter_attr_val_t val = esp_matter_nullable_int16(nullable<int16_t>((int16_t)v));
        attribute::update(s_temp_ep, TemperatureMeasurement::Id,
                          TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
    } else if (s == DM_SENSOR_HUMIDITY && s_hum_ep) {
        esp_matter_attr_val_t val = esp_matter_nullable_uint16(nullable<uint16_t>((uint16_t)v));
        attribute::update(s_hum_ep, RelativeHumidityMeasurement::Id,
                          RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
    }
}

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (!s_started || ev->src == DM_SRC_MATTER) {
        return;   /* before start: resync after start covers it */
    }
    const dm_event_t e = *ev;

    chip::DeviceLayer::SystemLayer().ScheduleLambda([e]() {
        switch (e.type) {
        case DM_EV_OUTPUT:
            push_output(e.idx, e.output);
            break;
        case DM_EV_INDICATOR:
            push_indicator(e.idx, e.indicator);
            break;
        case DM_EV_KEY:
            push_key(s_key_ep[e.idx], e.key.evt, e.key.count);
            break;
        case DM_EV_SENSOR:
            push_sensor((dm_sensor_t)e.idx, e.sensor_value);
            break;
        }
    });
}

/* ------------------------------------------------------------ setup */

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address changed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "commissioning failed, fail-safe timer expired");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "fabric removed");
        break;
    default:
        break;
    }
}

esp_err_t matter_app_init(void)
{
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attr_cb, identify_cb);
    if (!node) {
        return ESP_FAIL;
    }

    for (uint8_t i = 0; i < CONFIG_KEY_COUNT; i++) {
        generic_switch::config_t cfg;
        cfg.switch_cluster.feature_flags = cluster::switch_cluster::feature::momentary_switch::get_id();
        endpoint_t *ep = generic_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        if (!ep) {
            return ESP_FAIL;
        }
        cluster_t *sw = cluster::get(ep, Switch::Id);
        cluster::switch_cluster::feature::momentary_switch_release::add(sw);
        cluster::switch_cluster::feature::momentary_switch_long_press::add(sw);
        cluster::switch_cluster::feature::momentary_switch_multi_press::config_t msm;
        msm.multi_press_max = CONFIG_KEY_MULTI_PRESS_MAX;
        cluster::switch_cluster::feature::momentary_switch_multi_press::add(sw, &msm);
        s_key_ep[i] = endpoint::get_id(ep);
    }

    for (uint8_t i = 0; i < CONFIG_OUTPUT_COUNT; i++) {
        dimmable_light::config_t cfg;
        cfg.on_off.on_off = false;                              /* real state pushed after start */
        cfg.on_off_lighting.start_up_on_off = nullptr;          /* = previous; DM restores */
        cfg.level_control.current_level = DM_LEVEL_MAX;
        cfg.level_control_lighting.start_up_current_level = nullptr;
        endpoint_t *ep = dimmable_light::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        if (!ep) {
            return ESP_FAIL;
        }
        s_out_ep[i] = endpoint::get_id(ep);
        /* CurrentLevel changes rapidly during transitions: defer NVS writes. */
        attribute::set_deferred_persistence(
            attribute::get(s_out_ep[i], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id));
    }

    for (uint8_t i = 0; i < CONFIG_INDICATOR_COUNT; i++) {
        on_off_light::config_t cfg;
        cfg.on_off.on_off = false;
        endpoint_t *ep = on_off_light::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        if (!ep) {
            return ESP_FAIL;
        }
        s_ind_ep[i] = endpoint::get_id(ep);
    }

    if (sensors_present()) {
        temperature_sensor::config_t tcfg;
        tcfg.temperature_measurement.min_measured_value = nullable<int16_t>(-4000);
        tcfg.temperature_measurement.max_measured_value = nullable<int16_t>(12500);
        endpoint_t *t = temperature_sensor::create(node, &tcfg, ENDPOINT_FLAG_NONE, nullptr);
        humidity_sensor::config_t hcfg;
        hcfg.relative_humidity_measurement.min_measured_value = nullable<uint16_t>(0);
        hcfg.relative_humidity_measurement.max_measured_value = nullable<uint16_t>(10000);
        endpoint_t *h = humidity_sensor::create(node, &hcfg, ENDPOINT_FLAG_NONE, nullptr);
        s_temp_ep = t ? endpoint::get_id(t) : 0;
        s_hum_ep = h ? endpoint::get_id(h) : 0;
    }

    ESP_LOGI(TAG, "endpoints: %d key(s) from ep %u, %d output(s) from ep %u, %d LED(s), sensors %s",
             CONFIG_KEY_COUNT, s_key_ep[0], CONFIG_OUTPUT_COUNT, s_out_ep[0], CONFIG_INDICATOR_COUNT,
             sensors_present() ? "yes" : "no");
    return dm_register_listener(dm_listener, nullptr);
}

esp_err_t matter_app_start(void)
{
    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        return err;
    }
    s_started = true;

    /* Matter restored its own persisted attributes; the device model is the
     * source of truth, so overwrite them with the DM state. */
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        for (uint8_t i = 0; i < dm_output_count(); i++) {
            push_output(i, dm_output_get(i));
        }
        for (uint8_t i = 0; i < dm_indicator_count(); i++) {
            push_indicator(i, dm_indicator_get(i));
        }
    });

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif
    return ESP_OK;
}

void matter_app_factory_reset(void)
{
    app_settings_erase();
    /* Removes fabrics, erases Matter NVS and reboots. */
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() { esp_matter::factory_reset(); });
}

bool matter_app_commissioned(void)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    bool any = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return any;
}
