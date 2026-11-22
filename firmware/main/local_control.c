/*
 * Works without any controller or broker, so a wall switch always switches.
 * Per key (setting key_map[k] = output, -1 = events only):
 *
 *   single press   toggle output
 *   double press   output on at full level
 *   hold           dim up/down (direction alternates on each hold) until release
 *
 * Indicator k mirrors output key_map[k] when bit k of ind_follow_mask is set;
 * otherwise it is driven remotely (Matter on/off endpoint or MQTT).
 *
 * Key events are always forwarded to Matter/MQTT as well, so automations can
 * add scenes on top (e.g. triple press = "all off").
 */
#include "local_control.h"
#include "app_settings.h"
#include "device_model.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "local";

#define RAMP_PERIOD_US  (50 * 1000)
#define RAMP_STEP       6           /* ~2 s from min to max */

static esp_timer_handle_t s_ramp_timer;
static int8_t s_ramp_out = -1;
static int8_t s_ramp_dir[DM_MAX_OUTPUTS] = { 1, 1, 1, 1 };

static void ramp_cb(void *arg)
{
    int8_t o = s_ramp_out;

    if (o < 0) {
        return;
    }
    dm_output_state_t st = dm_output_get(o);
    int lvl = st.level + s_ramp_dir[o] * RAMP_STEP;

    if (lvl >= DM_LEVEL_MAX || lvl <= DM_LEVEL_MIN) {
        lvl = lvl >= DM_LEVEL_MAX ? DM_LEVEL_MAX : DM_LEVEL_MIN;
        esp_timer_stop(s_ramp_timer);
    }
    dm_output_set(o, true, (uint8_t)lvl, DM_SRC_LOCAL);
}

static void on_key(const dm_event_t *ev)
{
    int8_t out = app_cfg()->key_map[ev->idx];

    if (out < 0 || out >= dm_output_count()) {
        return;
    }

    switch (ev->key.evt) {
    case DM_KEY_SHORT_RELEASE:
        if (ev->key.count == 1) {
            dm_output_toggle(out, DM_SRC_LOCAL);
        }
        break;
    case DM_KEY_MULTI_PRESS_COMPLETE:
        if (ev->key.count == 2) {
            dm_output_set(out, true, DM_LEVEL_MAX, DM_SRC_LOCAL);
        }
        break;
    case DM_KEY_LONG_PRESS: {
        dm_output_state_t st = dm_output_get(out);
        if (!st.on) {
            dm_output_set(out, true, DM_LEVEL_MIN, DM_SRC_LOCAL);
            s_ramp_dir[out] = 1;
        } else {
            s_ramp_dir[out] = -s_ramp_dir[out];
            if (st.level >= DM_LEVEL_MAX) {
                s_ramp_dir[out] = -1;
            } else if (st.level <= DM_LEVEL_MIN) {
                s_ramp_dir[out] = 1;
            }
        }
        s_ramp_out = out;
        esp_timer_stop(s_ramp_timer);
        esp_timer_start_periodic(s_ramp_timer, RAMP_PERIOD_US);
        break;
    }
    case DM_KEY_LONG_RELEASE:
        esp_timer_stop(s_ramp_timer);
        s_ramp_out = -1;
        break;
    default:
        break;
    }
}

static void on_output(const dm_event_t *ev)
{
    const app_config_t *cfg = app_cfg();

    for (uint8_t k = 0; k < dm_indicator_count() && k < DM_MAX_KEYS; k++) {
        if ((cfg->ind_follow_mask & (1u << k)) && cfg->key_map[k] == ev->idx) {
            dm_indicator_set(k, ev->output.on, DM_SRC_LOCAL);
        }
    }
}

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (ev->type == DM_EV_KEY) {
        on_key(ev);
    } else if (ev->type == DM_EV_OUTPUT) {
        on_output(ev);
    }
}

esp_err_t local_control_init(void)
{
    const esp_timer_create_args_t args = { .callback = ramp_cb, .name = "ramp" };

    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_ramp_timer), TAG, "timer");
    return dm_register_listener(dm_listener, NULL);
}
