/* Assembly variant detection. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Decide the number of fitted outputs (call once, after app_settings_init()). */
void board_init(void);
uint8_t board_output_count(void);
const char *board_variant_source(void);   /* "override", "strap", "default" */
#ifdef __cplusplus
}
#endif
