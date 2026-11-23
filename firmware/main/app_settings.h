/* Provisioned configuration in NVS (namespace "sb"). Defaults from Kconfig. */
#pragma once

#include <stdint.h>
#include "device_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NAME_MAX   24
#define APP_URI_MAX    96
#define APP_USER_MAX   32
#define APP_PASS_MAX   64

typedef struct {
    char name[APP_NAME_MAX];        /* [A-Za-z0-9_-], used in topics + hostname */
    uint8_t plca_node_id;           /* 0 = coordinator, 255 = PLCA off */
    uint8_t plca_node_count;
    char mqtt_uri[APP_URI_MAX];     /* mqtt://host:1883 or mqtts://host:8883, "" = MQTT off */
    char mqtt_user[APP_USER_MAX];
    char mqtt_pass[APP_PASS_MAX];
    int8_t key_map[DM_MAX_KEYS];    /* key -> local output, -1 = events only */
    uint8_t ind_follow_mask;        /* bit n: indicator n mirrors output key_map[n] */
    int8_t output_override;         /* -1 = auto (strap / Kconfig), else fitted outputs */
} app_config_t;

esp_err_t app_settings_init(void);
const app_config_t *app_cfg(void);

esp_err_t app_settings_set_name(const char *name);
esp_err_t app_settings_set_plca(uint8_t node_id, uint8_t node_count);
esp_err_t app_settings_set_mqtt(const char *uri, const char *user, const char *pass);
esp_err_t app_settings_set_key_map(uint8_t key, int8_t output);
esp_err_t app_settings_set_ind_follow(uint8_t mask);
/* -1 = auto. Takes effect after reboot (Matter endpoints are created at boot). */
esp_err_t app_settings_set_output_override(int8_t count);

/* Erase app + device-model NVS namespaces. Matter fabrics are separate
 * (esp_matter::factory_reset()). */
esp_err_t app_settings_erase(void);

#ifdef __cplusplus
}
#endif
