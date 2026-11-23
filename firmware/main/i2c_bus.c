#include "i2c_bus.h"

#include "esp_check.h"
#include "sdkconfig.h"

static const char *TAG = "i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out)
{
    /* Called from app_main during init only (single thread), no lock needed. */
    if (!s_bus) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = -1,
            .sda_io_num = CONFIG_BOARD_I2C_SDA,
            .scl_io_num = CONFIG_BOARD_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,   /* rev A has 2.2k externals */
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "bus");
    }
    *out = s_bus;
    return ESP_OK;
}
