/* Offline behaviour: keys drive local outputs, indicators mirror outputs. */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t local_control_init(void);
#ifdef __cplusplus
}
#endif
