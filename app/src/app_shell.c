/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Provisioning / service shell:
 *
 *   sb show                     current config + channel states
 *   sb name <id>                device name (MQTT topic/HA id), [A-Za-z0-9_-]
 *   sb plca <node_id> [count]   PLCA node id (0 = coordinator, 255 = off)
 *   sb mqtt <host> [port]       broker
 *   sb ch <n> on|off|toggle     drive a channel
 *   sb factory-reset            erase config + state, reboot
 */
#include "app_settings.h"
#include "device_model.h"
#include "net_t1s.h"
#if defined(CONFIG_APP_MQTT)
#include "mqtt_app.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	const struct app_config *c = app_cfg();

	shell_print(sh, "name        : %s", c->name);
	shell_print(sh, "device id   : %s", net_t1s_device_id());
	shell_print(sh, "network     : %s", net_t1s_is_online() ? "online" : "offline");
	if (c->plca_node_id == 255) {
		shell_print(sh, "PLCA        : off (CSMA/CD)");
	} else {
		shell_print(sh, "PLCA        : node %u of %u", c->plca_node_id, c->plca_node_count);
	}
#if defined(CONFIG_APP_MQTT)
	shell_print(sh, "MQTT broker : %s:%u (%s)", c->mqtt_host, c->mqtt_port,
		    mqtt_app_is_connected() ? "connected" : "not connected");
#endif
	for (size_t i = 0; i < dm_channel_count(); i++) {
		shell_print(sh, "ch%u         : %s", (unsigned)i, dm_get(i) ? "ON" : "OFF");
	}
	return 0;
}

static int cmd_name(const struct shell *sh, size_t argc, char **argv)
{
	int err = app_settings_set_name(argv[1]);

	if (err) {
		shell_error(sh, "invalid name (%d)", err);
		return err;
	}
	shell_print(sh, "saved; reboot to apply to hostname/topics");
	return 0;
}

static int cmd_plca(const struct shell *sh, size_t argc, char **argv)
{
	long id = strtol(argv[1], NULL, 0);
	long cnt = argc > 2 ? strtol(argv[2], NULL, 0) : app_cfg()->plca_node_count;
	int err;

	if (id < 0 || id > 255 || cnt < 1 || cnt > 255) {
		shell_error(sh, "range: id 0..254 (255 = off), count 1..255");
		return -EINVAL;
	}
	err = app_settings_set_plca(id, cnt);
	if (err) {
		shell_error(sh, "invalid: node id must be < node count (%d)", err);
		return err;
	}
	err = net_t1s_apply_plca(id, cnt);
	shell_print(sh, "saved, %s", err ? "apply FAILED (reboot)" : "applied");
	return 0;
}

#if defined(CONFIG_APP_MQTT)
static int cmd_mqtt(const struct shell *sh, size_t argc, char **argv)
{
	long port = argc > 2 ? strtol(argv[2], NULL, 0) : app_cfg()->mqtt_port;
	int err;

	if (port < 1 || port > 65535) {
		return -EINVAL;
	}
	err = app_settings_set_mqtt(argv[1], port);
	if (err) {
		shell_error(sh, "invalid (%d)", err);
		return err;
	}
	mqtt_app_reconnect();
	shell_print(sh, "saved, reconnecting");
	return 0;
}
#endif

static int cmd_ch(const struct shell *sh, size_t argc, char **argv)
{
	long ch = strtol(argv[1], NULL, 0);
	int err;

	if (!strcmp(argv[2], "on")) {
		err = dm_set(ch, true, DM_SRC_SHELL);
	} else if (!strcmp(argv[2], "off")) {
		err = dm_set(ch, false, DM_SRC_SHELL);
	} else if (!strcmp(argv[2], "toggle")) {
		err = dm_toggle(ch, DM_SRC_SHELL);
	} else {
		err = -EINVAL;
	}
	if (err) {
		shell_error(sh, "usage: sb ch <n> on|off|toggle");
	}
	return err;
}

static int cmd_factory_reset(const struct shell *sh, size_t argc, char **argv)
{
	shell_warn(sh, "erasing configuration and rebooting");
	app_settings_factory_reset();
	k_sleep(K_MSEC(200));
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sb_cmds,
	SHELL_CMD_ARG(show, NULL, "Show configuration and state", cmd_show, 1, 0),
	SHELL_CMD_ARG(name, NULL, "<id> Set device name", cmd_name, 2, 0),
	SHELL_CMD_ARG(plca, NULL, "<node_id> [node_count] Set PLCA (255 = off)", cmd_plca, 2, 1),
#if defined(CONFIG_APP_MQTT)
	SHELL_CMD_ARG(mqtt, NULL, "<host> [port] Set MQTT broker", cmd_mqtt, 2, 1),
#endif
	SHELL_CMD_ARG(ch, NULL, "<n> on|off|toggle", cmd_ch, 3, 0),
	SHELL_CMD_ARG(factory-reset, NULL, "Erase config and reboot", cmd_factory_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sb, &sb_cmds, "Wired Switchboard commands", NULL);
