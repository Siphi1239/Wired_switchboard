#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Create the data model (endpoints) and register the DM listener.
 * Call after drivers/sensors are probed and before dm_init(). */
esp_err_t matter_app_init(void);
/* Start the Matter stack (this also brings up Ethernet via the hook). */
esp_err_t matter_app_start(void);
/* Erase Matter fabrics + app settings and reboot. Thread safe. */
void matter_app_factory_reset(void);
bool matter_app_commissioned(void);
#ifdef __cplusplus
}
#endif
