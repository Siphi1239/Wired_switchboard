#include "outputs.h"
#include "board.h"
#include "device_model.h"

#include <math.h>
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "outputs";

#define PWM_MODE     LEDC_LOW_SPEED_MODE   /* the only mode on ESP32-C6 */
#define PWM_TIMER    LEDC_TIMER_0
#define PWM_RES      LEDC_TIMER_12_BIT
#define PWM_MAX      ((1u << 12) - 1)

static const int s_gpios[DM_MAX_OUTPUTS] = {
    CONFIG_OUTPUT_GPIO_0, CONFIG_OUTPUT_GPIO_1, CONFIG_OUTPUT_GPIO_2, CONFIG_OUTPUT_GPIO_3,
};

/* Perceptual (gamma 2.2) mapping of Matter level 1..254 to duty. */
static uint32_t level_to_duty(const dm_output_state_t *st)
{
    if (!st->on) {
        return 0;
    }
    float x = (float)st->level / DM_LEVEL_MAX;
    uint32_t d = (uint32_t)lroundf(powf(x, 2.2f) * PWM_MAX);
    return d ? d : 1;
}

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (ev->type != DM_EV_OUTPUT || ev->idx >= board_output_count()) {
        return;
    }
    uint32_t duty = level_to_duty(&ev->output);
    int fade = ev->src == DM_SRC_BOOT ? 0 : CONFIG_OUTPUT_FADE_MS;

    if (fade) {
        ledc_set_fade_with_time(PWM_MODE, (ledc_channel_t)ev->idx, duty, fade);
        ledc_fade_start(PWM_MODE, (ledc_channel_t)ev->idx, LEDC_FADE_NO_WAIT);
    } else {
        ledc_set_duty(PWM_MODE, (ledc_channel_t)ev->idx, duty);
        ledc_update_duty(PWM_MODE, (ledc_channel_t)ev->idx);
    }
}

esp_err_t outputs_init(void)
{
    if (board_output_count() == 0) {
        return ESP_OK;
    }

    const ledc_timer_config_t tcfg = {
        .speed_mode = PWM_MODE,
        .duty_resolution = PWM_RES,
        .timer_num = PWM_TIMER,
        .freq_hz = CONFIG_OUTPUT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "timer");

    for (int i = 0; i < board_output_count(); i++) {
        const ledc_channel_config_t ccfg = {
            .gpio_num = s_gpios[i],
            .speed_mode = PWM_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = PWM_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "channel %d", i);
    }
    ESP_RETURN_ON_ERROR(ledc_fade_func_install(0), TAG, "fade");
    ESP_RETURN_ON_ERROR(dm_register_listener(dm_listener, NULL), TAG, "listener");

    ESP_LOGI(TAG, "%u PWM output(s) @ %d Hz, 12 bit", board_output_count(), CONFIG_OUTPUT_PWM_FREQ_HZ);
    return ESP_OK;
}
