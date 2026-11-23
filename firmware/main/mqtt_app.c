/*
 * MQTT front-end with Home Assistant discovery.
 *
 * base = <prefix>/<name>
 *   <base>/status               online|offline           retained, LWT
 *   <base>/out/<n>/state        {"state":"ON","brightness":128}  retained
 *   <base>/out/<n>/set          same JSON (HA light, JSON schema, brightness_scale 254)
 *   <base>/ind/<n>/state|set    ON|OFF
 *   <base>/key/<n>/event        {"event_type":"single|double|triple|long_press|long_release"}
 *   <base>/sensor/temperature   21.5      retained
 *   <base>/sensor/humidity      45.0      retained
 *
 * Publishing from the device-model task uses esp_mqtt_client_enqueue()
 * (non-blocking, thread safe); the esp-mqtt task sends.
 */
#include "mqtt_app.h"
#include "app_settings.h"
#include "device_model.h"
#include "eth_t1s.h"
#include "power_mon.h"
#include "sensors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#if CONFIG_APP_MQTT

static const char *TAG = "mqtt";

#define TOPIC_MAX 128

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_base[64];
static char s_lwt[80];

/* ------------------------------------------------------------ publish helpers */

static void pub(const char *topic, const char *payload, bool retain)
{
    if (s_client && s_connected) {
        esp_mqtt_client_enqueue(s_client, topic, payload, 0, 0, retain, true);
    }
}

static void pub_output(uint8_t i)
{
    char topic[TOPIC_MAX], payload[64];
    dm_output_state_t st = dm_output_get(i);

    snprintf(topic, sizeof(topic), "%s/out/%u/state", s_base, i);
    snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%u}", st.on ? "ON" : "OFF", st.level);
    pub(topic, payload, true);
}

static void pub_indicator(uint8_t i)
{
    char topic[TOPIC_MAX];

    snprintf(topic, sizeof(topic), "%s/ind/%u/state", s_base, i);
    pub(topic, dm_indicator_get(i) ? "ON" : "OFF", true);
}

static void pub_sensor(dm_sensor_t s)
{
    char topic[TOPIC_MAX], payload[16];
    int32_t v;

    if (!dm_sensor_get(s, &v)) {
        return;
    }
    static const char *const names[DM_SENSOR_COUNT] = { "temperature", "humidity", "bus_voltage" };
    snprintf(topic, sizeof(topic), "%s/sensor/%s", s_base, names[s]);
    if (s == DM_SENSOR_BUS_VOLTAGE) {
        snprintf(payload, sizeof(payload), "%ld.%02ld", v / 1000, (v % 1000) / 10);
    } else {
        snprintf(payload, sizeof(payload), "%s%ld.%02ld", v < 0 ? "-" : "", labs(v) / 100, labs(v) % 100);
    }
    pub(topic, payload, true);
}

static const char *key_event_name(const dm_event_t *ev)
{
    switch (ev->key.evt) {
    case DM_KEY_MULTI_PRESS_COMPLETE:
        return ev->key.count == 1 ? "single" : ev->key.count == 2 ? "double" : "triple";
    case DM_KEY_LONG_PRESS:
        return "long_press";
    case DM_KEY_LONG_RELEASE:
        return "long_release";
    default:
        return NULL;   /* InitialPress/ShortRelease/Ongoing: Matter only */
    }
}

/* ------------------------------------------------------------ HA discovery */

static void discovery(void)
{
    const char *id = eth_t1s_device_id();
    const char *name = app_cfg()->name;
    char topic[TOPIC_MAX];
    char dev[320];
    char *buf = malloc(896);

    if (!buf) {
        return;
    }
    snprintf(dev, sizeof(dev),
             "\"avty_t\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"DIY\","
             "\"mdl\":\"Wired Switchboard (10BASE-T1S)\",\"sw\":\"%s\"}",
             s_lwt, id, name, esp_app_get_description()->version);

    for (uint8_t i = 0; i < dm_output_count(); i++) {
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/light/%s/out%u/config", id, i);
        snprintf(buf, 896,
                 "{\"name\":\"Output %u\",\"uniq_id\":\"%s_out%u\",\"schema\":\"json\","
                 "\"stat_t\":\"%s/out/%u/state\",\"cmd_t\":\"%s/out/%u/set\","
                 "\"brightness\":true,\"brightness_scale\":254,%s}",
                 i + 1, id, i, s_base, i, s_base, i, dev);
        pub(topic, buf, true);
    }
    for (uint8_t i = 0; i < dm_indicator_count(); i++) {
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/switch/%s/ind%u/config", id, i);
        snprintf(buf, 896,
                 "{\"name\":\"Key %u LED\",\"uniq_id\":\"%s_ind%u\",\"ent_cat\":\"config\","
                 "\"stat_t\":\"%s/ind/%u/state\",\"cmd_t\":\"%s/ind/%u/set\",%s}",
                 i + 1, id, i, s_base, i, s_base, i, dev);
        pub(topic, buf, true);
    }
    for (uint8_t i = 0; i < dm_key_count(); i++) {
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/event/%s/key%u/config", id, i);
        snprintf(buf, 896,
                 "{\"name\":\"Key %u\",\"uniq_id\":\"%s_key%u\",\"dev_cla\":\"button\","
                 "\"evt_typ\":[\"single\",\"double\",\"triple\",\"long_press\",\"long_release\"],"
                 "\"stat_t\":\"%s/key/%u/event\",%s}",
                 i + 1, id, i, s_base, i, dev);
        pub(topic, buf, true);
    }
    if (sensors_present()) {
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/sensor/%s/temperature/config", id);
        snprintf(buf, 896,
                 "{\"name\":\"Temperature\",\"uniq_id\":\"%s_temp\",\"dev_cla\":\"temperature\","
                 "\"unit_of_meas\":\"\xC2\xB0" "C\",\"stat_cla\":\"measurement\","
                 "\"stat_t\":\"%s/sensor/temperature\",%s}",
                 id, s_base, dev);
        pub(topic, buf, true);
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/sensor/%s/humidity/config", id);
        snprintf(buf, 896,
                 "{\"name\":\"Humidity\",\"uniq_id\":\"%s_hum\",\"dev_cla\":\"humidity\","
                 "\"unit_of_meas\":\"%%\",\"stat_cla\":\"measurement\","
                 "\"stat_t\":\"%s/sensor/humidity\",%s}",
                 id, s_base, dev);
        pub(topic, buf, true);
    }
    if (power_mon_present()) {
        snprintf(topic, sizeof(topic), CONFIG_APP_MQTT_HA_PREFIX "/sensor/%s/bus_voltage/config", id);
        snprintf(buf, 896,
                 "{\"name\":\"Bus voltage\",\"uniq_id\":\"%s_vbus\",\"dev_cla\":\"voltage\","
                 "\"unit_of_meas\":\"V\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\","
                 "\"stat_t\":\"%s/sensor/bus_voltage\",%s}",
                 id, s_base, dev);
        pub(topic, buf, true);
    }
    free(buf);
}

/* ------------------------------------------------------------ commands */

/* "<base>/<kind>/<n>/set" -> n (0..9) or -1 */
static int parse_idx(const char *t, int tlen, const char *kind)
{
    char pfx[96];
    int plen = snprintf(pfx, sizeof(pfx), "%s/%s/", s_base, kind);

    if (tlen != plen + 5 || strncmp(t, pfx, plen) || strncmp(t + plen + 1, "/set", 4)) {
        return -1;
    }
    char c = t[plen];
    return (c >= '0' && c <= '9') ? c - '0' : -1;
}

static void handle_output_cmd(uint8_t i, const char *data, int len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);

    if (!j) {
        /* Also accept plain ON/OFF for simple clients. */
        if (len == 2 && !strncmp(data, "ON", 2)) {
            dm_output_set_on(i, true, DM_SRC_MQTT);
        } else if (len == 3 && !strncmp(data, "OFF", 3)) {
            dm_output_set_on(i, false, DM_SRC_MQTT);
        }
        return;
    }
    const cJSON *state = cJSON_GetObjectItem(j, "state");
    const cJSON *bri = cJSON_GetObjectItem(j, "brightness");
    dm_output_state_t cur = dm_output_get(i);
    bool on = cur.on;
    uint8_t level = 0;

    if (cJSON_IsString(state)) {
        on = !strcmp(state->valuestring, "ON");
    }
    if (cJSON_IsNumber(bri)) {
        int b = bri->valueint;
        level = b < DM_LEVEL_MIN ? DM_LEVEL_MIN : b > DM_LEVEL_MAX ? DM_LEVEL_MAX : b;
        if (!cJSON_IsString(state)) {
            on = true;
        }
    }
    dm_output_set(i, on, level, DM_SRC_MQTT);
    cJSON_Delete(j);
}

static void on_data(const esp_mqtt_event_t *e)
{
    int i;

    if ((i = parse_idx(e->topic, e->topic_len, "out")) >= 0 && i < dm_output_count()) {
        handle_output_cmd(i, e->data, e->data_len);
    } else if ((i = parse_idx(e->topic, e->topic_len, "ind")) >= 0 && i < dm_indicator_count()) {
        bool on = e->data_len == 2 && !strncmp(e->data, "ON", 2);
        dm_indicator_set(i, on, DM_SRC_MQTT);
    } else {
        ESP_LOGW(TAG, "unhandled topic %.*s", e->topic_len, e->topic);
    }
}

static void on_connected(void)
{
    char sub[TOPIC_MAX];

    s_connected = true;
    ESP_LOGI(TAG, "connected");
    pub(s_lwt, "online", true);

    snprintf(sub, sizeof(sub), "%s/out/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, sub, 1);
    snprintf(sub, sizeof(sub), "%s/ind/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, sub, 1);

    discovery();
    for (uint8_t i = 0; i < dm_output_count(); i++) {
        pub_output(i);
    }
    for (uint8_t i = 0; i < dm_indicator_count(); i++) {
        pub_indicator(i);
    }
    pub_sensor(DM_SENSOR_TEMPERATURE);
    pub_sensor(DM_SENSOR_HUMIDITY);
    pub_sensor(DM_SENSOR_BUS_VOLTAGE);
}

static void mqtt_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        on_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        /* Commands are small; ignore fragmented (multi-chunk) messages. */
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len) {
            on_data(e);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "error type %d", e->error_handle ? (int)e->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ DM listener */

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (!s_connected) {
        return;   /* full state is published on (re)connect */
    }
    switch (ev->type) {
    case DM_EV_OUTPUT:
        pub_output(ev->idx);
        break;
    case DM_EV_INDICATOR:
        pub_indicator(ev->idx);
        break;
    case DM_EV_SENSOR:
        pub_sensor((dm_sensor_t)ev->idx);
        break;
    case DM_EV_KEY: {
        const char *name = key_event_name(ev);
        if (name) {
            char topic[TOPIC_MAX], payload[48];
            snprintf(topic, sizeof(topic), "%s/key/%u/event", s_base, ev->idx);
            snprintf(payload, sizeof(payload), "{\"event_type\":\"%s\"}", name);
            pub(topic, payload, false);
        }
        break;
    }
    }
}

/* ------------------------------------------------------------ lifecycle */

static void client_start(void)
{
    const app_config_t *cfg = app_cfg();
    static char client_id[48];

    if (cfg->mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "no broker configured, MQTT off");
        return;
    }
    snprintf(s_base, sizeof(s_base), CONFIG_APP_MQTT_TOPIC_PREFIX "/%s", cfg->name);
    snprintf(s_lwt, sizeof(s_lwt), "%s/status", s_base);
    snprintf(client_id, sizeof(client_id), "%s-%s", cfg->name, eth_t1s_device_id());

    const esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = cfg->mqtt_uri,
        .credentials.client_id = client_id,
        .credentials.username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .session.keepalive = 60,
        .session.last_will = { .topic = s_lwt, .msg = "offline", .qos = 1, .retain = 1 },
        /* Jittered reconnect: many nodes on one segment lose power together. */
        .network.reconnect_timeout_ms = 3000 + (esp_random() % 4000),
        .buffer.size = 1024,
    };
    s_client = esp_mqtt_client_init(&mcfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_cb, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "broker %s, base topic %s", cfg->mqtt_uri, s_base);
}

static void start_task(void *arg)
{
    /* Wait for DHCP: the broker is IPv4 in most homes. */
    eth_t1s_wait_ipv4(portMAX_DELAY);
    client_start();
    vTaskDelete(NULL);
}

esp_err_t mqtt_app_init(void)
{
    ESP_RETURN_ON_ERROR(dm_register_listener(dm_listener, NULL), TAG, "listener");
    return xTaskCreate(start_task, "mqtt_start", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void mqtt_app_restart(void)
{
    if (s_client) {
        s_connected = false;
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    client_start();
}

bool mqtt_app_connected(void)
{
    return s_connected;
}

#else /* !CONFIG_APP_MQTT */

esp_err_t mqtt_app_init(void) { return ESP_OK; }
void mqtt_app_restart(void) {}
bool mqtt_app_connected(void) { return false; }

#endif
