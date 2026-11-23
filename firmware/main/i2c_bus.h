/* Shared I2C master bus (SHT4x, LP5012), created on first use. */
#pragma once
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out);
#ifdef __cplusplus
}
#endif
