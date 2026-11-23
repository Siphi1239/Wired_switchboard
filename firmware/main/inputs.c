/*
 * Keys are sampled every 10 ms (esp_timer), debounced over 3 samples, and
 * turned into the event sequence of the Matter Switch cluster with features
 * MS | MSR | MSL | MSM:
 *
 *   single:  InitialPress, ShortRelease, MultiPressComplete(1)
 *   double:  InitialPress, ShortRelease, MultiPressOngoing(2), ShortRelease,
 *            MultiPressComplete(2)
 *   hold:    InitialPress, LongPress, LongRelease
 *
 * Polling instead of GPIO interrupts keeps the code simple and immune to
 * contact bounce on long wall-switch cables; 4 GPIO reads / 10 ms is free.
 */
#include "inputs.h"
#include "device_model.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "inputs";

#define SAMPLE_MS          10
#define DEBOUNCE_SAMPLES   3
#define FACTORY_RESET_MS   10000

typedef enum { K_IDLE, K_PRESSED, K_LONG, K_WAIT } kstate_t;

typedef struct {
    int gpio;
    bool stable;          /* debounced level, true = pressed */
    uint8_t same_cnt;
    kstate_t st;
    uint8_t count;
    int64_t t_edge_ms;
} key_ctx_t;

static key_ctx_t s_keys[DM_MAX_KEYS];
static uint8_t s_n;
static esp_timer_handle_t s_timer;
static inputs_factory_reset_cb_t s_reset_cb;
static int64_t s_reset_pressed_since = -1;

static const int s_gpios[DM_MAX_KEYS] = {
    CONFIG_KEY_GPIO_0, CONFIG_KEY_GPIO_1, CONFIG_KEY_GPIO_2, CONFIG_KEY_GPIO_3,
};

static bool read_pressed(int gpio)
{
    int lvl = gpio_get_level(gpio);
#if CONFIG_KEY_ACTIVE_LOW
    return lvl == 0;
#else
    return lvl != 0;
#endif
}

static void on_press(uint8_t i, key_ctx_t *k, int64_t now)
{
    k->t_edge_ms = now;
    if (k->st == K_WAIT) {
        k->count++;
        k->st = K_PRESSED;
        dm_key_event(i, DM_KEY_MULTI_PRESS_ONGOING, k->count);
    } else {
        k->count = 1;
        k->st = K_PRESSED;
        dm_key_event(i, DM_KEY_INITIAL_PRESS, 1);
    }
}

static void on_release(uint8_t i, key_ctx_t *k, int64_t now)
{
    k->t_edge_ms = now;
    if (k->st == K_LONG) {
        dm_key_event(i, DM_KEY_LONG_RELEASE, 1);
        k->st = K_IDLE;
        return;
    }
    dm_key_event(i, DM_KEY_SHORT_RELEASE, k->count);
    if (k->count >= CONFIG_KEY_MULTI_PRESS_MAX) {
        dm_key_event(i, DM_KEY_MULTI_PRESS_COMPLETE, k->count);
        k->st = K_IDLE;
    } else {
        k->st = K_WAIT;
    }
}

static void sample_cb(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;

    for (uint8_t i = 0; i < s_n; i++) {
        key_ctx_t *k = &s_keys[i];
        bool p = read_pressed(k->gpio);

        if (p != k->stable) {
            if (++k->same_cnt >= DEBOUNCE_SAMPLES) {
                k->stable = p;
                k->same_cnt = 0;
                if (p) {
                    on_press(i, k, now);
                } else {
                    on_release(i, k, now);
                }
            }
        } else {
            k->same_cnt = 0;
        }

        if (k->st == K_PRESSED && k->count == 1 && now - k->t_edge_ms >= CONFIG_KEY_LONG_PRESS_MS) {
            k->st = K_LONG;
            dm_key_event(i, DM_KEY_LONG_PRESS, 1);
        } else if (k->st == K_WAIT && now - k->t_edge_ms >= CONFIG_KEY_MULTI_PRESS_WINDOW_MS) {
            dm_key_event(i, DM_KEY_MULTI_PRESS_COMPLETE, k->count);
            k->st = K_IDLE;
        }
    }

#if CONFIG_KEY_FACTORY_RESET_GPIO >= 0
    bool reset_held = gpio_get_level(CONFIG_KEY_FACTORY_RESET_GPIO) == 0;
#else
    /* No dedicated button (rev A): keys 0 and 1 held together. */
    bool reset_held = s_n >= 2 && s_keys[0].stable && s_keys[1].stable;
#endif
    if (reset_held) {
        if (s_reset_pressed_since < 0) {
            s_reset_pressed_since = now;
        } else if (now - s_reset_pressed_since >= FACTORY_RESET_MS && s_reset_cb) {
            ESP_LOGW(TAG, "factory reset button held %d s", FACTORY_RESET_MS / 1000);
            s_reset_pressed_since = INT64_MAX / 2; /* fire once */
            s_reset_cb();
        }
    } else {
        s_reset_pressed_since = -1;
    }
}

esp_err_t inputs_init(inputs_factory_reset_cb_t on_factory_reset)
{
    uint64_t mask = 0;

    s_n = CONFIG_KEY_COUNT;
    s_reset_cb = on_factory_reset;

    for (uint8_t i = 0; i < s_n; i++) {
        s_keys[i] = (key_ctx_t){ .gpio = s_gpios[i], .st = K_IDLE };
        mask |= 1ULL << s_gpios[i];
    }

    if (mask) {
        const gpio_config_t io = {
            .pin_bit_mask = mask,
            .mode = GPIO_MODE_INPUT,
#if CONFIG_KEY_ACTIVE_LOW
            .pull_up_en = GPIO_PULLUP_ENABLE,
#else
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config");
    }

#if CONFIG_KEY_FACTORY_RESET_GPIO >= 0
    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << CONFIG_KEY_FACTORY_RESET_GPIO,  /* BOOT button: active low */
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset gpio config");
#endif

    const esp_timer_create_args_t args = { .callback = sample_cb, .name = "keys" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, SAMPLE_MS * 1000), TAG, "timer start");

    ESP_LOGI(TAG, "%u key(s)", s_n);
    return ESP_OK;
}
