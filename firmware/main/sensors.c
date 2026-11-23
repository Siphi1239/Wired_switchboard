/*
 * SHT4x (SHT40/41/45) on I2C, address 0x44. High-precision single shot
 * (0xFD), 6 bytes: T[2] CRC RH[2] CRC.
 *
 * Note on placement: on a PoDL-powered wall node the buck converter and
 * MCU warm the PCB. Thermally isolate the sensor (slot in the PCB, at the
 * bottom of the enclosure) or apply an offset - see docs/architecture.md.
 */
#include "sensors.h"
#include "device_model.h"

#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SENSOR_SHT4X

static const char *TAG = "sensors";

#define SHT4X_ADDR        0x44
#define SHT4X_MEAS_HIGH   0xFD
#define I2C_TIMEOUT_MS    50

static i2c_master_dev_handle_t s_dev;
static bool s_present;

static uint8_t crc8(const uint8_t *d, int len)
{
    uint8_t crc = 0xFF;

    for (int i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht4x_read(int32_t *t_centi, int32_t *rh_centi)
{
    uint8_t cmd = SHT4X_MEAS_HIGH;
    uint8_t rx[6];

    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &cmd, 1, I2C_TIMEOUT_MS), TAG, "tx");
    vTaskDelay(pdMS_TO_TICKS(10));   /* max 8.3 ms */
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, rx, sizeof(rx), I2C_TIMEOUT_MS), TAG, "rx");
    if (crc8(rx, 2) != rx[2] || crc8(rx + 3, 2) != rx[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t t_raw = ((uint32_t)rx[0] << 8) | rx[1];
    uint32_t rh_raw = ((uint32_t)rx[3] << 8) | rx[4];

    /* T = -45 + 175 * raw / 65535 ; RH = -6 + 125 * raw / 65535 (in 1/100) */
    *t_centi = -4500 + (int32_t)((17500u * t_raw) / 65535u);
    int32_t rh = -600 + (int32_t)((12500u * rh_raw) / 65535u);
    *rh_centi = rh < 0 ? 0 : rh > 10000 ? 10000 : rh;
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    for (;;) {
        int32_t t, rh;
        esp_err_t err = sht4x_read(&t, &rh);

        if (err == ESP_OK) {
            /* Round to 0.1 degC / 0.5 %RH: avoids reporting noise to Matter/MQTT. */
            dm_sensor_update(DM_SENSOR_TEMPERATURE, ((t + (t >= 0 ? 5 : -5)) / 10) * 10);
            dm_sensor_update(DM_SENSOR_HUMIDITY, ((rh + 25) / 50) * 50);
        } else {
            ESP_LOGW(TAG, "SHT4x read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SENSOR_PERIOD_S * 1000));
    }
}

esp_err_t sensors_init(void)
{
    i2c_master_bus_handle_t bus;

    ESP_RETURN_ON_ERROR(i2c_bus_get(&bus), TAG, "i2c bus");

    if (i2c_master_probe(bus, SHT4X_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "no SHT4x at 0x%02x - sensor endpoints disabled", SHT4X_ADDR);
        return ESP_OK;
    }
    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT4X_ADDR,
        .scl_speed_hz = CONFIG_BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dcfg, &s_dev), TAG, "add dev");
    s_present = true;

    if (xTaskCreate(sensor_task, "sensors", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SHT4x found, period %d s", CONFIG_SENSOR_PERIOD_S);
    return ESP_OK;
}

bool sensors_present(void)
{
    return s_present;
}

#else

esp_err_t sensors_init(void)
{
    return ESP_OK;
}

bool sensors_present(void)
{
    return false;
}

#endif
