#include "device_model.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "dm";

#define MAX_LISTENERS     6
#define QUEUE_LEN         32
#define SAVE_DELAY_US     (2 * 1000 * 1000)  /* debounce NVS writes (flash wear) */
#define NVS_NS            "dm"

static uint8_t s_keys, s_outputs, s_indicators;
static dm_output_state_t s_out[DM_MAX_OUTPUTS];
static bool s_ind[DM_MAX_INDICATORS];
static int32_t s_sensor[DM_SENSOR_COUNT];
static bool s_sensor_valid[DM_SENSOR_COUNT];

static struct {
    dm_listener_t cb;
    void *ctx;
} s_listeners[MAX_LISTENERS];
static uint8_t s_n_listeners;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static esp_timer_handle_t s_save_timer;

/* ------------------------------------------------------------ persistence */

static void save_cb(void *arg)
{
    nvs_handle_t h;
    dm_output_state_t snap[DM_MAX_OUTPUTS];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(snap, s_out, sizeof(snap));
    xSemaphoreGive(s_lock);

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "out", snap, sizeof(snap));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void restore(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_out);

    for (int i = 0; i < DM_MAX_OUTPUTS; i++) {
        s_out[i] = (dm_output_state_t){ .on = false, .level = DM_LEVEL_MAX };
    }
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "out", s_out, &len) != ESP_OK || len != sizeof(s_out)) {
        for (int i = 0; i < DM_MAX_OUTPUTS; i++) {
            s_out[i] = (dm_output_state_t){ .on = false, .level = DM_LEVEL_MAX };
        }
    }
    nvs_close(h);
    for (int i = 0; i < DM_MAX_OUTPUTS; i++) {
        if (s_out[i].level < DM_LEVEL_MIN || s_out[i].level > DM_LEVEL_MAX) {
            s_out[i].level = DM_LEVEL_MAX;
        }
    }
}

/* ------------------------------------------------------------ dispatch */

static void post(const dm_event_t *ev)
{
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped type %d idx %u", ev->type, ev->idx);
    }
}

static void dm_task(void *arg)
{
    dm_event_t ev;

    for (;;) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        for (uint8_t i = 0; i < s_n_listeners; i++) {
            s_listeners[i].cb(&ev, s_listeners[i].ctx);
        }
    }
}

esp_err_t dm_init(uint8_t n_keys, uint8_t n_outputs, uint8_t n_indicators)
{
    if (n_keys > DM_MAX_KEYS || n_outputs > DM_MAX_OUTPUTS || n_indicators > DM_MAX_INDICATORS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_keys = n_keys;
    s_outputs = n_outputs;
    s_indicators = n_indicators;

    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(dm_event_t));
    if (!s_lock || !s_queue) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = { .callback = save_cb, .name = "dm_save" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_save_timer));

    restore();
    if (xTaskCreate(dm_task, "dm", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Announce the restored state so drivers drive it and front-ends learn
     * it. Listeners must therefore be registered before dm_init(). */
    for (uint8_t i = 0; i < s_outputs; i++) {
        dm_event_t ev = { .type = DM_EV_OUTPUT, .idx = i, .src = DM_SRC_BOOT, .output = s_out[i] };
        ESP_LOGI(TAG, "out%u restored: %s level %u", i, s_out[i].on ? "ON" : "OFF", s_out[i].level);
        post(&ev);
    }
    return ESP_OK;
}

esp_err_t dm_register_listener(dm_listener_t cb, void *ctx)
{
    if (s_n_listeners >= MAX_LISTENERS) {
        return ESP_ERR_NO_MEM;
    }
    s_listeners[s_n_listeners].cb = cb;
    s_listeners[s_n_listeners].ctx = ctx;
    s_n_listeners++;
    return ESP_OK;
}

uint8_t dm_key_count(void) { return s_keys; }
uint8_t dm_output_count(void) { return s_outputs; }
uint8_t dm_indicator_count(void) { return s_indicators; }

/* ------------------------------------------------------------ outputs */

esp_err_t dm_output_set(uint8_t idx, bool on, uint8_t level, dm_source_t src)
{
    if (idx >= s_outputs) {
        return ESP_ERR_INVALID_ARG;
    }
    if (level != 0) {
        level = level < DM_LEVEL_MIN ? DM_LEVEL_MIN : level > DM_LEVEL_MAX ? DM_LEVEL_MAX : level;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    dm_output_state_t old = s_out[idx];
    s_out[idx].on = on;
    if (level) {
        s_out[idx].level = level;
    }
    dm_output_state_t now = s_out[idx];
    xSemaphoreGive(s_lock);

    if (old.on == now.on && old.level == now.level) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "out%u -> %s level %u (%s)", idx, now.on ? "ON" : "OFF", now.level, dm_source_str(src));
    dm_event_t ev = { .type = DM_EV_OUTPUT, .idx = idx, .src = src, .output = now };
    post(&ev);
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
    return ESP_OK;
}

esp_err_t dm_output_set_on(uint8_t idx, bool on, dm_source_t src)
{
    return dm_output_set(idx, on, 0, src);
}

esp_err_t dm_output_set_level(uint8_t idx, uint8_t level, dm_source_t src)
{
    /* A level without explicit on/off keeps the on state (Matter MoveToLevel
     * semantics); use dm_output_set() for "...WithOnOff". */
    return dm_output_set(idx, dm_output_get(idx).on, level ? level : DM_LEVEL_MIN, src);
}

esp_err_t dm_output_toggle(uint8_t idx, dm_source_t src)
{
    return dm_output_set(idx, !dm_output_get(idx).on, 0, src);
}

dm_output_state_t dm_output_get(uint8_t idx)
{
    dm_output_state_t st = { 0 };

    if (idx < s_outputs) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        st = s_out[idx];
        xSemaphoreGive(s_lock);
    }
    return st;
}

/* ------------------------------------------------------------ indicators */

esp_err_t dm_indicator_set(uint8_t idx, bool on, dm_source_t src)
{
    if (idx >= s_indicators) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_ind[idx] != on;
    s_ind[idx] = on;
    xSemaphoreGive(s_lock);

    if (changed) {
        dm_event_t ev = { .type = DM_EV_INDICATOR, .idx = idx, .src = src, .indicator = on };
        post(&ev);
    }
    return ESP_OK;
}

bool dm_indicator_get(uint8_t idx)
{
    return idx < s_indicators && s_ind[idx];
}

/* ------------------------------------------------------------ keys / sensors */

void dm_key_event(uint8_t idx, dm_key_evt_t evt, uint8_t count)
{
    if (idx >= s_keys) {
        return;
    }
    ESP_LOGI(TAG, "key%u %s (%u)", idx, dm_key_evt_str(evt), count);
    dm_event_t ev = { .type = DM_EV_KEY, .idx = idx, .src = DM_SRC_LOCAL,
                      .key = { .evt = evt, .count = count } };
    post(&ev);
}

void dm_sensor_update(dm_sensor_t sensor, int32_t value)
{
    if (sensor >= DM_SENSOR_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = !s_sensor_valid[sensor] || s_sensor[sensor] != value;
    s_sensor[sensor] = value;
    s_sensor_valid[sensor] = true;
    xSemaphoreGive(s_lock);

    if (changed) {
        dm_event_t ev = { .type = DM_EV_SENSOR, .idx = (uint8_t)sensor, .src = DM_SRC_LOCAL,
                          .sensor_value = value };
        post(&ev);
    }
}

bool dm_sensor_get(dm_sensor_t sensor, int32_t *value)
{
    if (sensor >= DM_SENSOR_COUNT || !s_sensor_valid[sensor]) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *value = s_sensor[sensor];
    xSemaphoreGive(s_lock);
    return true;
}

const char *dm_source_str(dm_source_t src)
{
    switch (src) {
    case DM_SRC_BOOT:    return "boot";
    case DM_SRC_LOCAL:   return "local";
    case DM_SRC_MQTT:    return "mqtt";
    case DM_SRC_MATTER:  return "matter";
    case DM_SRC_CONSOLE: return "console";
    }
    return "?";
}

const char *dm_key_evt_str(dm_key_evt_t evt)
{
    switch (evt) {
    case DM_KEY_INITIAL_PRESS:        return "initial_press";
    case DM_KEY_SHORT_RELEASE:        return "short_release";
    case DM_KEY_LONG_PRESS:           return "long_press";
    case DM_KEY_LONG_RELEASE:         return "long_release";
    case DM_KEY_MULTI_PRESS_ONGOING:  return "multi_press_ongoing";
    case DM_KEY_MULTI_PRESS_COMPLETE: return "multi_press_complete";
    }
    return "?";
}
