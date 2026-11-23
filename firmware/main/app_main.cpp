/*
 * Wired Switchboard - ESP32-C6 + LAN8651 10BASE-T1S, powered over the data line.
 *
 * Init order:
 *   1. NVS, settings, assembly variant (number of fitted outputs)
 *   2. drivers + logic register as device-model listeners (outputs, indicators,
 *      local control, MQTT); sensors are probed so Matter knows what exists
 *   3. Matter data model (endpoints) + its DM listener
 *   4. dm_init(): restores outputs -> drivers drive them
 *   5. inputs (keys start producing events)
 *   6. Matter start -> Ethernet hook brings up the LAN8651, DHCP/SLAAC, BLE
 *      commissioning window if not commissioned; MQTT connects on IPv4
 */
#include "app_console.h"
#include "app_settings.h"
#include "board.h"
#include "device_model.h"
#include "eth_t1s.h"
#include "indicators.h"
#include "inputs.h"
#include "local_control.h"
#include "matter_app.h"
#include "mqtt_app.h"
#include "outputs.h"
#include "power_mon.h"
#include "sensors.h"

#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

static const char *TAG = "app";

#define CHECK(x)                                                                 \
    do {                                                                         \
        esp_err_t _e = (x);                                                      \
        if (_e != ESP_OK) {                                                      \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(_e));             \
            abort();                                                             \
        }                                                                        \
    } while (0)

static void on_factory_reset_button(void)
{
    matter_app_factory_reset();
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Wired Switchboard %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    CHECK(err);

    CHECK(app_settings_init());
    board_init();
    CHECK(eth_t1s_preinit());

    CHECK(outputs_init());
    CHECK(indicators_init());
    CHECK(sensors_init());
    CHECK(local_control_init());
    CHECK(mqtt_app_init());
    CHECK(matter_app_init());

    CHECK(dm_init(CONFIG_KEY_COUNT, board_output_count(), CONFIG_INDICATOR_COUNT));
    CHECK(inputs_init(on_factory_reset_button));
    CHECK(power_mon_init());

    CHECK(matter_app_start());
#if CONFIG_ENABLE_CHIP_SHELL
    CHECK(app_console_register());
#endif
}
