/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Provisioned configuration, persisted in NVS under "sb/...".
 * Defaults come from Kconfig; values are changed via the `sb` shell
 * (and later via MQTT/Matter if desired).
 */
#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdint.h>

#define APP_NAME_MAX   32
#define APP_HOST_MAX   64

struct app_config {
	char name[APP_NAME_MAX];       /* topic / discovery id */
	uint8_t plca_node_id;          /* 0 = coordinator, 255 = PLCA off */
	uint8_t plca_node_count;
	char mqtt_host[APP_HOST_MAX];
	uint16_t mqtt_port;
};

/* Registers handler, loads all settings (also for other subsystems). */
int app_settings_init(void);

/* Read-only view of the current config. */
const struct app_config *app_cfg(void);

int app_settings_set_name(const char *name);
int app_settings_set_plca(uint8_t node_id, uint8_t node_count);
int app_settings_set_mqtt(const char *host, uint16_t port);

/* Erase all persisted settings (provisioning + channel state). */
int app_settings_factory_reset(void);

#endif
