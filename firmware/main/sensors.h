/* Environmental sensors feeding the device model. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t sensors_init(void);
bool sensors_present(void);
#ifdef __cplusplus
}
#endif
