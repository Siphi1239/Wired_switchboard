#include "app_settings.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";
#define NS "sb"

static app_config_t s_cfg;

static void defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.name, CONFIG_APP_DEVICE_NAME, sizeof(s_cfg.name));
    s_cfg.plca_node_id = CONFIG_T1S_PLCA_NODE_ID;
    s_cfg.plca_node_count = CONFIG_T1S_PLCA_NODE_COUNT;
#if CONFIG_APP_MQTT
    strlcpy(s_cfg.mqtt_uri, CONFIG_APP_MQTT_BROKER_URI, sizeof(s_cfg.mqtt_uri));
#endif
    for (int i = 0; i < DM_MAX_KEYS; i++) {
        s_cfg.key_map[i] = (i < CONFIG_OUTPUT_COUNT) ? i : -1;
    }
    s_cfg.ind_follow_mask = 0xFF;
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    char tmp[APP_URI_MAX];

    if (size <= sizeof(tmp) && nvs_get_str(h, key, tmp, &len) == ESP_OK) {
        strlcpy(dst, tmp, size);
    }
}

esp_err_t app_settings_init(void)
{
    nvs_handle_t h;
    size_t len;

    defaults();
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config, using defaults");
        return ESP_OK;
    }
    load_str(h, "name", s_cfg.name, sizeof(s_cfg.name));
    load_str(h, "mqtt_uri", s_cfg.mqtt_uri, sizeof(s_cfg.mqtt_uri));
    load_str(h, "mqtt_user", s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user));
    load_str(h, "mqtt_pass", s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass));
    nvs_get_u8(h, "plca_id", &s_cfg.plca_node_id);
    nvs_get_u8(h, "plca_cnt", &s_cfg.plca_node_count);
    nvs_get_u8(h, "ind_follow", &s_cfg.ind_follow_mask);
    len = sizeof(s_cfg.key_map);
    nvs_get_blob(h, "key_map", s_cfg.key_map, &len);
    nvs_close(h);
    return ESP_OK;
}

const app_config_t *app_cfg(void)
{
    return &s_cfg;
}

static esp_err_t save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool valid_name(const char *s)
{
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_')) {
            return false;
        }
    }
    return true;
}

esp_err_t app_settings_set_name(const char *name)
{
    ESP_RETURN_ON_FALSE(strlen(name) < sizeof(s_cfg.name) && valid_name(name), ESP_ERR_INVALID_ARG, TAG,
                        "name: 1..%d chars of [A-Za-z0-9_-]", APP_NAME_MAX - 1);
    strlcpy(s_cfg.name, name, sizeof(s_cfg.name));
    return save_str("name", s_cfg.name);
}

esp_err_t app_settings_set_plca(uint8_t node_id, uint8_t node_count)
{
    ESP_RETURN_ON_FALSE(node_count > 0 && (node_id == 255 || node_id < node_count), ESP_ERR_INVALID_ARG, TAG,
                        "node id must be < node count (or 255 = off)");
    s_cfg.plca_node_id = node_id;
    s_cfg.plca_node_count = node_count;
    ESP_RETURN_ON_ERROR(save_u8("plca_id", node_id), TAG, "save");
    return save_u8("plca_cnt", node_count);
}

esp_err_t app_settings_set_mqtt(const char *uri, const char *user, const char *pass)
{
    ESP_RETURN_ON_FALSE(strlen(uri) < sizeof(s_cfg.mqtt_uri) && strlen(user) < sizeof(s_cfg.mqtt_user) &&
                        strlen(pass) < sizeof(s_cfg.mqtt_pass), ESP_ERR_INVALID_SIZE, TAG, "too long");
    strlcpy(s_cfg.mqtt_uri, uri, sizeof(s_cfg.mqtt_uri));
    strlcpy(s_cfg.mqtt_user, user, sizeof(s_cfg.mqtt_user));
    strlcpy(s_cfg.mqtt_pass, pass, sizeof(s_cfg.mqtt_pass));
    ESP_RETURN_ON_ERROR(save_str("mqtt_uri", uri), TAG, "save");
    ESP_RETURN_ON_ERROR(save_str("mqtt_user", user), TAG, "save");
    /* Plain NVS. Enable NVS encryption (nvs_keys partition exists) for production. */
    return save_str("mqtt_pass", pass);
}

esp_err_t app_settings_set_key_map(uint8_t key, int8_t output)
{
    nvs_handle_t h;

    ESP_RETURN_ON_FALSE(key < DM_MAX_KEYS && output >= -1 && output < DM_MAX_OUTPUTS, ESP_ERR_INVALID_ARG, TAG,
                        "range");
    s_cfg.key_map[key] = output;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, "key_map", s_cfg.key_map, sizeof(s_cfg.key_map));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_settings_set_ind_follow(uint8_t mask)
{
    s_cfg.ind_follow_mask = mask;
    return save_u8("ind_follow", mask);
}

esp_err_t app_settings_erase(void)
{
    static const char *const spaces[] = { NS, "dm" };

    for (int i = 0; i < 2; i++) {
        nvs_handle_t h;
        if (nvs_open(spaces[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    defaults();
    return ESP_OK;
}
