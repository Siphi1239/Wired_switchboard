#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Register DM listener (call before dm_init) and start the client once IPv4 is up. */
esp_err_t mqtt_app_init(void);
/* Re-read broker settings and reconnect. */
void mqtt_app_restart(void);
bool mqtt_app_connected(void);
#ifdef __cplusplus
}
#endif
