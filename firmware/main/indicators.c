/*
 * One WS2812 pixel per key. "On" = warm white, "off" = faint blue glow so
 * the keys can be found in the dark. Colours are a placeholder for a later
 * per-key colour setting (Matter ColorControl or MQTT RGB).
 */
#include "indicators.h"
#include "device_model.h"

#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "indicators";

static led_strip_handle_t s_strip;

static void set_pixel(uint8_t idx, bool on)
{
    const uint32_t b = CONFIG_INDICATOR_BRIGHTNESS;

    if (on) {
        led_strip_set_pixel(s_strip, idx, b, (b * 3) / 4, b / 3);
    } else {
        led_strip_set_pixel(s_strip, idx, 0, 0, b / 16 ? b / 16 : 1);
    }
}

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (ev->type != DM_EV_INDICATOR || ev->idx >= CONFIG_INDICATOR_COUNT) {
        return;
    }
    set_pixel(ev->idx, ev->indicator);
    led_strip_refresh(s_strip);
}

esp_err_t indicators_init(void)
{
    if (CONFIG_INDICATOR_COUNT == 0) {
        return ESP_OK;
    }

    const led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_INDICATOR_GPIO,
        .max_leds = CONFIG_INDICATOR_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&cfg, &rmt, &s_strip), TAG, "led strip");

    for (uint8_t i = 0; i < CONFIG_INDICATOR_COUNT; i++) {
        set_pixel(i, false);
    }
    led_strip_refresh(s_strip);

    ESP_RETURN_ON_ERROR(dm_register_listener(dm_listener, NULL), TAG, "listener");
    ESP_LOGI(TAG, "%d indicator LED(s) on GPIO%d", CONFIG_INDICATOR_COUNT, CONFIG_INDICATOR_GPIO);
    return ESP_OK;
}
