/* Low-voltage dimmable outputs (LEDC PWM -> MOSFET low-side driver). */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Register as device-model listener; call before dm_init(). */
esp_err_t outputs_init(void);
#ifdef __cplusplus
}
#endif
