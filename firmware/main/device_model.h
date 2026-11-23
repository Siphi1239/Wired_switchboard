/*
 * Device model: the single owner of the node's state.
 *
 *   keys        momentary wall-switch inputs  -> events only (no state)
 *   outputs     low-voltage PWM channels      -> on/off + level (1..254, Matter scale)
 *   indicators  key LEDs                      -> on/off
 *   sensors     temperature / humidity        -> value in 1/100 units
 *
 * Front-ends (Matter, MQTT), local logic and drivers never talk to each other.
 * They call the dm_* setters and receive dm_event_t through listeners, which
 * run on the device-model task (never on the Matter or MQTT task).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DM_MAX_KEYS        4
#define DM_MAX_OUTPUTS     4
#define DM_MAX_INDICATORS  8
#define DM_LEVEL_MIN       1
#define DM_LEVEL_MAX       254

typedef enum {
    DM_SRC_BOOT,
    DM_SRC_LOCAL,     /* key press, local_control */
    DM_SRC_MQTT,
    DM_SRC_MATTER,
    DM_SRC_CONSOLE,
} dm_source_t;

/* Matter Switch cluster event model (momentary switch, MSR | MSL | MSM). */
typedef enum {
    DM_KEY_INITIAL_PRESS,
    DM_KEY_SHORT_RELEASE,
    DM_KEY_LONG_PRESS,
    DM_KEY_LONG_RELEASE,
    DM_KEY_MULTI_PRESS_ONGOING,
    DM_KEY_MULTI_PRESS_COMPLETE,
} dm_key_evt_t;

typedef enum {
    DM_SENSOR_TEMPERATURE,  /* 0.01 degC */
    DM_SENSOR_HUMIDITY,     /* 0.01 %RH  */
    DM_SENSOR_BUS_VOLTAGE,  /* mV (power_mon) */
    DM_SENSOR_COUNT,
} dm_sensor_t;

typedef enum {
    DM_EV_OUTPUT,
    DM_EV_INDICATOR,
    DM_EV_KEY,
    DM_EV_SENSOR,
} dm_event_type_t;

typedef struct {
    bool on;
    uint8_t level;   /* last level, kept while off */
} dm_output_state_t;

typedef struct {
    dm_event_type_t type;
    uint8_t idx;
    dm_source_t src;
    union {
        dm_output_state_t output;
        bool indicator;
        struct {
            dm_key_evt_t evt;
            uint8_t count;   /* press count for MULTI_PRESS_*, else 1 */
        } key;
        int32_t sensor_value;
    };
} dm_event_t;

typedef void (*dm_listener_t)(const dm_event_t *ev, void *ctx);

esp_err_t dm_init(uint8_t n_keys, uint8_t n_outputs, uint8_t n_indicators);
esp_err_t dm_register_listener(dm_listener_t cb, void *ctx);

uint8_t dm_key_count(void);
uint8_t dm_output_count(void);
uint8_t dm_indicator_count(void);

/* Outputs. Events are emitted only on actual change, which also breaks
 * update loops between front-ends. level 0 = keep current level. */
esp_err_t dm_output_set(uint8_t idx, bool on, uint8_t level, dm_source_t src);
esp_err_t dm_output_set_on(uint8_t idx, bool on, dm_source_t src);
esp_err_t dm_output_set_level(uint8_t idx, uint8_t level, dm_source_t src);
esp_err_t dm_output_toggle(uint8_t idx, dm_source_t src);
dm_output_state_t dm_output_get(uint8_t idx);

esp_err_t dm_indicator_set(uint8_t idx, bool on, dm_source_t src);
bool dm_indicator_get(uint8_t idx);

/* Called by the input driver. */
void dm_key_event(uint8_t idx, dm_key_evt_t evt, uint8_t count);

/* Called by the sensor driver. */
void dm_sensor_update(dm_sensor_t sensor, int32_t value);
bool dm_sensor_get(dm_sensor_t sensor, int32_t *value);

/* Write pending output state to NVS now (brown-out). Safe from any task. */
void dm_flush(void);

const char *dm_source_str(dm_source_t src);
const char *dm_key_evt_str(dm_key_evt_t evt);

#ifdef __cplusplus
}
#endif
