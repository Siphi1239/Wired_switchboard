/* Bus (PoDL) voltage monitor: reporting + brown-out state save. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t power_mon_init(void);
bool power_mon_present(void);
int32_t power_mon_min_mv(void);
#ifdef __cplusplus
}
#endif
