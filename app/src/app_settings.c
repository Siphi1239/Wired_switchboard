/* SPDX-License-Identifier: Apache-2.0 */
#include "app_settings.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(cfg, CONFIG_APP_LOG_LEVEL);

static struct app_config cfg = {
	.name = CONFIG_APP_DEVICE_NAME_DEFAULT,
	.plca_node_id = CONFIG_APP_PLCA_NODE_ID_DEFAULT,
	.plca_node_count = CONFIG_APP_PLCA_NODE_COUNT_DEFAULT,
#if defined(CONFIG_APP_MQTT)
	.mqtt_host = CONFIG_APP_MQTT_BROKER_DEFAULT,
	.mqtt_port = CONFIG_APP_MQTT_PORT_DEFAULT,
#endif
};

static int read_str(char *dst, size_t dst_size, size_t len, settings_read_cb read_cb, void *arg)
{
	if (len >= dst_size) {
		return -EINVAL;
	}
	ssize_t rc = read_cb(arg, dst, len);

	if (rc < 0) {
		return rc;
	}
	dst[rc] = '\0';
	return 0;
}

static int read_val(void *dst, size_t size, size_t len, settings_read_cb read_cb, void *arg)
{
	if (len != size) {
		return -EINVAL;
	}
	return read_cb(arg, dst, size) == size ? 0 : -EIO;
}

static int sb_set(const char *key, size_t len, settings_read_cb read_cb, void *arg)
{
	if (!strcmp(key, "name")) {
		return read_str(cfg.name, sizeof(cfg.name), len, read_cb, arg);
	}
	if (!strcmp(key, "plca_id")) {
		return read_val(&cfg.plca_node_id, sizeof(cfg.plca_node_id), len, read_cb, arg);
	}
	if (!strcmp(key, "plca_cnt")) {
		return read_val(&cfg.plca_node_count, sizeof(cfg.plca_node_count), len, read_cb,
				arg);
	}
	if (!strcmp(key, "mqtt_host")) {
		return read_str(cfg.mqtt_host, sizeof(cfg.mqtt_host), len, read_cb, arg);
	}
	if (!strcmp(key, "mqtt_port")) {
		return read_val(&cfg.mqtt_port, sizeof(cfg.mqtt_port), len, read_cb, arg);
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(sb, "sb", NULL, sb_set, NULL, NULL);

int app_settings_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings init failed (%d)", err);
		return err;
	}
	return settings_load();
}

const struct app_config *app_cfg(void)
{
	return &cfg;
}

static bool valid_name(const char *s)
{
	/* Used verbatim in MQTT topics: no wildcards, separators or spaces. */
	if (!*s) {
		return false;
	}
	for (; *s; s++) {
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) {
			return false;
		}
	}
	return true;
}

int app_settings_set_name(const char *name)
{
	if (strlen(name) >= sizeof(cfg.name) || !valid_name(name)) {
		return -EINVAL;
	}
	strcpy(cfg.name, name);
	return settings_save_one("sb/name", cfg.name, strlen(cfg.name));
}

int app_settings_set_plca(uint8_t node_id, uint8_t node_count)
{
	if (node_count == 0 || (node_id != 255 && node_id >= node_count)) {
		return -EINVAL;
	}
	cfg.plca_node_id = node_id;
	cfg.plca_node_count = node_count;

	int err = settings_save_one("sb/plca_id", &cfg.plca_node_id, sizeof(cfg.plca_node_id));

	return err ? err : settings_save_one("sb/plca_cnt", &cfg.plca_node_count,
					     sizeof(cfg.plca_node_count));
}

int app_settings_set_mqtt(const char *host, uint16_t port)
{
	if (strlen(host) >= sizeof(cfg.mqtt_host) || port == 0) {
		return -EINVAL;
	}
	strcpy(cfg.mqtt_host, host);
	cfg.mqtt_port = port;

	int err = settings_save_one("sb/mqtt_host", cfg.mqtt_host, strlen(cfg.mqtt_host));

	return err ? err : settings_save_one("sb/mqtt_port", &cfg.mqtt_port, sizeof(cfg.mqtt_port));
}

int app_settings_factory_reset(void)
{
	/*
	 * Delete our keys through the settings API instead of erasing the
	 * partition directly: the partition name differs between Partition
	 * Manager (NCS) and plain DT fixed-partitions builds.
	 * Phase 2: also call chip::Server::ScheduleFactoryReset() for Matter.
	 */
	static const char *const keys[] = {
		"sb/name", "sb/plca_id", "sb/plca_cnt", "sb/mqtt_host", "sb/mqtt_port", "dm/state",
	};
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		int err = settings_delete(keys[i]);

		if (err && err != -ENOENT) {
			ret = err;
		}
	}
	return ret;
}
