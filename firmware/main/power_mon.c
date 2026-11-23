/*
 * Samples the bus voltage (behind the eFuse) every 50 ms:
 *   - reports it to the device model when it moved by > REPORT_DELTA
 *     (MQTT diagnostic sensor, console)
 *   - < WARN: logs once, flushes output state to NVS
 *   - < CRIT: flushes output state to NVS (brown-out imminent)
 * Hysteresis 1 V on both thresholds.
 *
 * Board note: the bulk capacitor behind the eFuse (≥ 100 µF) must carry the
 * node (~25 mA at 3.3 V) through one NVS write (< 20 ms) after CRIT.
 */
#include "power_mon.h"
#include "device_model.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_POWER_MON_ADC_GPIO >= 0

#include <stdlib.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pwr";

#define PERIOD_MS       50
#define HYST_MV         1000

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_ch;
static int32_t s_min_mv = INT32_MAX;
static bool s_present;

static int32_t read_bus_mv(void)
{
    int raw, mv, sum = 0;

    for (int i = 0; i < 4; i++) {
        if (adc_oneshot_read(s_adc, s_ch, &raw) != ESP_OK) {
            return -1;
        }
        if (s_cali) {
            adc_cali_raw_to_voltage(s_cali, raw, &mv);
        } else {
            mv = raw * 3100 / 4095;   /* uncalibrated, 12 dB attenuation */
        }
        sum += mv;
    }
    int64_t pin_mv = sum / 4;
    return (int32_t)(pin_mv * (CONFIG_POWER_MON_R_TOP_KOHM + CONFIG_POWER_MON_R_BOTTOM_KOHM) /
                     CONFIG_POWER_MON_R_BOTTOM_KOHM);
}

static void power_task(void *arg)
{
    int32_t last_reported = -100000;
    bool warn = false, crit = false;

    for (;;) {
        int32_t mv = read_bus_mv();

        if (mv >= 0) {
            if (mv < s_min_mv) {
                s_min_mv = mv;
            }
            if (!crit && mv < CONFIG_POWER_MON_CRIT_MV) {
                crit = true;
                dm_flush();
                ESP_LOGE(TAG, "bus %ld mV: brown-out, state saved", (long)mv);
            } else if (crit && mv > CONFIG_POWER_MON_CRIT_MV + HYST_MV) {
                crit = false;
            }
            if (!warn && mv < CONFIG_POWER_MON_WARN_MV) {
                warn = true;
                dm_flush();
                ESP_LOGW(TAG, "bus %ld mV: undervoltage", (long)mv);
            } else if (warn && mv > CONFIG_POWER_MON_WARN_MV + HYST_MV) {
                warn = false;
                ESP_LOGI(TAG, "bus %ld mV: recovered", (long)mv);
            }
            if (abs(mv - last_reported) > CONFIG_POWER_MON_REPORT_DELTA_MV) {
                last_reported = mv;
                dm_sensor_update(DM_SENSOR_BUS_VOLTAGE, mv);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t power_mon_init(void)
{
    adc_unit_t unit;

    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(CONFIG_POWER_MON_ADC_GPIO, &unit, &s_ch), TAG,
                        "GPIO%d is not an ADC pin", CONFIG_POWER_MON_ADC_GPIO);
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "adc unit");
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_ch, &ccfg), TAG, "adc chan");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration in eFuse, readings uncalibrated");
    }
#endif
    s_present = true;
    if (xTaskCreate(power_task, "pwr_mon", 2560, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "bus monitor on GPIO%d, warn < %d mV, brown-out < %d mV", CONFIG_POWER_MON_ADC_GPIO,
             CONFIG_POWER_MON_WARN_MV, CONFIG_POWER_MON_CRIT_MV);
    return ESP_OK;
}

bool power_mon_present(void)
{
    return s_present;
}

int32_t power_mon_min_mv(void)
{
    return s_min_mv == INT32_MAX ? 0 : s_min_mv;
}

#else /* no sense pin (DevKit) */

esp_err_t power_mon_init(void)
{
    return ESP_OK;
}

bool power_mon_present(void)
{
    return false;
}

int32_t power_mon_min_mv(void)
{
    return 0;
}

#endif
