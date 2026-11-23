/*
 * One firmware image for both assembly variants:
 *   keypad            - output block not fitted (DNP)
 *   keypad + outputs  - CONFIG_OUTPUT_COUNT PWM outputs
 *
 * Priority: console override (NVS "sb/outputs") > variant-ID strap > Kconfig.
 * Strap: resistor divider on CONFIG_BOARD_VARIANT_ADC_GPIO,
 *   top 10k to 3V3, bottom 0R   -> ~0 V    -> keypad
 *   top 10k,        bottom 10k  -> ~1.65 V -> outputs fitted
 */
#include "board.h"
#include "app_settings.h"

#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_BOARD_VARIANT_ADC_GPIO >= 0
#include "esp_adc/adc_oneshot.h"
#endif

static const char *TAG = "board";

static uint8_t s_outputs = CONFIG_OUTPUT_COUNT;
static const char *s_source = "default";

#if CONFIG_BOARD_VARIANT_ADC_GPIO >= 0
static int read_strap_mv(void)
{
    adc_unit_t unit;
    adc_channel_t ch;
    adc_oneshot_unit_handle_t h;
    int raw = -1, sum = 0;

    if (adc_oneshot_io_to_channel(CONFIG_BOARD_VARIANT_ADC_GPIO, &unit, &ch) != ESP_OK) {
        return -1;
    }
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &h) != ESP_OK) {
        return -1;
    }
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(h, ch, &ccfg);
    for (int i = 0; i < 8; i++) {
        adc_oneshot_read(h, ch, &raw);
        sum += raw;
    }
    adc_oneshot_del_unit(h);
    /* Uncalibrated is plenty for a 0 V vs VDD/2 decision (12 dB: ~0..3.3 V). */
    return (sum / 8) * 3300 / 4095;
}
#endif

void board_init(void)
{
    int8_t ovr = app_cfg()->output_override;

    if (ovr >= 0) {
        s_outputs = ovr > CONFIG_OUTPUT_COUNT ? CONFIG_OUTPUT_COUNT : ovr;
        s_source = "override";
    } else {
#if CONFIG_BOARD_VARIANT_ADC_GPIO >= 0
        int mv = read_strap_mv();
        if (mv >= 0) {
            s_outputs = mv > 800 ? CONFIG_OUTPUT_COUNT : 0;
            s_source = "strap";
            ESP_LOGI(TAG, "variant strap %d mV", mv);
        }
#endif
    }
    ESP_LOGI(TAG, "%s variant, %u output(s) (%s)", s_outputs ? "output" : "keypad", s_outputs, s_source);
}

uint8_t board_output_count(void)
{
    return s_outputs;
}

const char *board_variant_source(void)
{
    return s_source;
}
