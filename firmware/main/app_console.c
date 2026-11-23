/*
 *   sb show                          config, network, states
 *   sb name <id>                     device name ([A-Za-z0-9_-]), reboot to apply
 *   sb plca <node_id> [node_count]   0 = coordinator, 255 = off; applied live
 *   sb mqtt <uri> [user] [pass]      e.g. mqtt://192.168.1.10:1883 ; "sb mqtt off"
 *   sb map <key> <output|-1>         local key -> output binding
 *   sb follow <mask>                 indicators mirroring their key's output (hex/dec)
 *   sb out <n> on|off|toggle|<1-254>
 *   sb led <n> on|off
 *   sb outputs auto|<0..N>          fitted outputs override, reboot to apply
 *   sb factory-reset                 erase app config AND Matter fabrics, reboot
 */
#include "app_console.h"
#include "app_settings.h"
#include "board.h"
#include "device_model.h"
#include "eth_t1s.h"
#include "matter_app.h"
#include "mqtt_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"

static int usage(void)
{
    printf("usage: sb show | name <id> | plca <id> [cnt] | mqtt <uri>|off [user] [pass] |\n"
           "          map <key> <out|-1> | follow <mask> | out <n> on|off|toggle|<1-254> |\n"
           "          led <n> on|off | outputs auto|<n> | factory-reset\n");
    return 1;
}

static void show(void)
{
    const app_config_t *c = app_cfg();

    printf("name      : %s\n", c->name);
    printf("device id : %s\n", eth_t1s_device_id());
    printf("variant   : %u output(s) (%s)\n", board_output_count(), board_variant_source());
    eth_t1s_print_status();
    printf("matter    : %s\n", matter_app_commissioned() ? "commissioned" : "not commissioned");
    printf("mqtt      : %s (%s)\n", c->mqtt_uri[0] ? c->mqtt_uri : "off",
           mqtt_app_connected() ? "connected" : "not connected");
    for (uint8_t i = 0; i < dm_output_count(); i++) {
        dm_output_state_t st = dm_output_get(i);
        printf("out%u      : %s level %u\n", i, st.on ? "ON " : "OFF", st.level);
    }
    for (uint8_t i = 0; i < dm_key_count(); i++) {
        char target[16];
        if (c->key_map[i] < 0) {
            snprintf(target, sizeof(target), "events only");
        } else {
            snprintf(target, sizeof(target), "out%d", c->key_map[i]);
        }
        printf("key%u      : -> %s, LED %s (%s)\n", i, target, dm_indicator_get(i) ? "on" : "off",
               (c->ind_follow_mask & (1u << i)) ? "follows" : "remote");
    }
    int32_t v;
    if (dm_sensor_get(DM_SENSOR_TEMPERATURE, &v)) {
        printf("temp      : %ld.%02ld C\n", v / 100, labs(v) % 100);
    }
    if (dm_sensor_get(DM_SENSOR_HUMIDITY, &v)) {
        printf("humidity  : %ld.%02ld %%\n", v / 100, v % 100);
    }
}

static int check(esp_err_t err)
{
    if (err != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("ok\n");
    return 0;
}

static int cmd_sb(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    const char *sub = argv[1];

    if (!strcmp(sub, "show")) {
        show();
        return 0;
    }
    if (!strcmp(sub, "name") && argc == 3) {
        int r = check(app_settings_set_name(argv[2]));
        if (!r) {
            printf("reboot to apply to hostname / MQTT topics\n");
        }
        return r;
    }
    if (!strcmp(sub, "plca") && argc >= 3) {
        long id = strtol(argv[2], NULL, 0);
        long cnt = argc > 3 ? strtol(argv[3], NULL, 0) : app_cfg()->plca_node_count;
        if (id < 0 || id > 255 || cnt < 1 || cnt > 255) {
            return usage();
        }
        if (check(app_settings_set_plca(id, cnt))) {
            return 1;
        }
        return check(eth_t1s_set_plca(id, cnt));
    }
    if (!strcmp(sub, "mqtt") && argc >= 3) {
        const char *uri = strcmp(argv[2], "off") ? argv[2] : "";
        if (check(app_settings_set_mqtt(uri, argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : ""))) {
            return 1;
        }
        mqtt_app_restart();
        return 0;
    }
    if (!strcmp(sub, "map") && argc == 4) {
        return check(app_settings_set_key_map(strtol(argv[2], NULL, 0), strtol(argv[3], NULL, 0)));
    }
    if (!strcmp(sub, "follow") && argc == 3) {
        return check(app_settings_set_ind_follow(strtol(argv[2], NULL, 0)));
    }
    if (!strcmp(sub, "out") && argc == 4) {
        uint8_t n = strtol(argv[2], NULL, 0);
        const char *v = argv[3];
        if (!strcmp(v, "on")) {
            return check(dm_output_set_on(n, true, DM_SRC_CONSOLE));
        } else if (!strcmp(v, "off")) {
            return check(dm_output_set_on(n, false, DM_SRC_CONSOLE));
        } else if (!strcmp(v, "toggle")) {
            return check(dm_output_toggle(n, DM_SRC_CONSOLE));
        }
        long lvl = strtol(v, NULL, 0);
        if (lvl < DM_LEVEL_MIN || lvl > DM_LEVEL_MAX) {
            return usage();
        }
        return check(dm_output_set(n, true, lvl, DM_SRC_CONSOLE));
    }
    if (!strcmp(sub, "led") && argc == 4) {
        return check(dm_indicator_set(strtol(argv[2], NULL, 0), !strcmp(argv[3], "on"), DM_SRC_CONSOLE));
    }
    if (!strcmp(sub, "outputs") && argc == 3) {
        int8_t n = strcmp(argv[2], "auto") ? (int8_t)strtol(argv[2], NULL, 0) : -1;
        int r = check(app_settings_set_output_override(n));
        if (!r) {
            printf("reboot to apply. Matter endpoints change: re-interview or re-commission the node\n");
        }
        return r;
    }
    if (!strcmp(sub, "factory-reset")) {
        printf("erasing configuration and Matter fabrics, rebooting...\n");
        matter_app_factory_reset();
        return 0;
    }
    return usage();
}

esp_err_t app_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sb",
        .help = "Wired Switchboard provisioning (sb show for status)",
        .func = cmd_sb,
    };
    return esp_console_cmd_register(&cmd);
}
