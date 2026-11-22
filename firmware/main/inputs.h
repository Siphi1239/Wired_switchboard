/* Wall-switch keys: debounced sampling + Matter momentary-switch event model. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*inputs_factory_reset_cb_t)(void);

esp_err_t inputs_init(inputs_factory_reset_cb_t on_factory_reset);

#ifdef __cplusplus
}
#endif
