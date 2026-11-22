#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Register the `sb` provisioning command (after the console REPL exists). */
esp_err_t app_console_register(void);
#ifdef __cplusplus
}
#endif
