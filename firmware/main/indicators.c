/*
 * Key indicator LEDs. Two backends:
 *   WS2812 (RMT)  - DevKit on-board LED / development
 *   TI LP5012     - rev A front board: 4 RGB modules over I2C, 3.3 V,
 *                   12-bit dithered PWM (linear, same colours as WS2812)
 *
 * Colours: "on" = warm white, "off" = faint blue glow so the keys can be
 * found in the dark.
 */
#include "indicators.h"
#include "device_model.h"

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "indicators";

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static rgb_t colour(bool on)
{
    const uint8_t b = CONFIG_INDICATOR_BRIGHTNESS;

    if (on) {
        return (rgb_t){ b, (uint8_t)((b * 3) / 4), (uint8_t)(b / 3) };
    }
    return (rgb_t){ 0, 0, (uint8_t)(b / 16 ? b / 16 : 1) };
}

/* ------------------------------------------------------------ WS2812 */
#if CONFIG_INDICATOR_DRIVER_WS2812
#include "led_strip.h"

static led_strip_handle_t s_strip;

static esp_err_t hw_init(void)
{
    const led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_INDICATOR_GPIO,
        .max_leds = CONFIG_INDICATOR_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    return led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
}

static void hw_set(uint8_t idx, rgb_t c)
{
    led_strip_set_pixel(s_strip, idx, c.r, c.g, c.b);
    led_strip_refresh(s_strip);
}

/* ------------------------------------------------------------ LP5012 */
#elif CONFIG_INDICATOR_DRIVER_LP5012
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

/* Register map: TI LP5009/LP5012 datasheet; matches Linux leds-lp50xx.c */
#define LP5012_DEV_CFG0      0x00
#define LP5012_DEV_CFG1      0x01
#define LP5012_LED_CFG0      0x02    /* bank mode per module, 0 = independent */
#define LP5012_LED0_BRT      0x07    /* LED0..3_BRIGHTNESS 0x07..0x0A */
#define LP5012_OUT0_CLR      0x0B    /* OUT0..11_COLOR 0x0B..0x16 */
#define LP5012_RESET         0x17
#define LP5012_CHIP_EN       0x40    /* DEV_CFG0 bit 6 */
/* DEV_CFG1: power save | auto increment | PWM dithering (0x1C). Log scale
 * (bit 5, on by default) is disabled so colour values behave like WS2812.
 * Max current option 0 = 25.5 mA full scale; the colour value scales it. */
#define LP5012_CFG1_VALUE    0x1C

static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(s_dev, buf, len, 50);
}

static esp_err_t hw_init(void)
{
    i2c_master_bus_handle_t bus;

#if CONFIG_LP5012_GPIO_EN >= 0
    const gpio_config_t en = {
        .pin_bit_mask = 1ULL << CONFIG_LP5012_GPIO_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&en), TAG, "en gpio");
    gpio_set_level(CONFIG_LP5012_GPIO_EN, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(1));                       /* t_start 500 us */

    ESP_RETURN_ON_ERROR(i2c_bus_get(&bus), TAG, "i2c bus");
    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_LP5012_I2C_ADDR,
        .scl_speed_hz = CONFIG_BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dcfg, &s_dev), TAG, "add dev");

    const uint8_t rst[] = { LP5012_RESET, 0xFF };
    ESP_RETURN_ON_ERROR(wr(rst, sizeof(rst)), TAG, "LP5012 not responding at 0x%02x", CONFIG_LP5012_I2C_ADDR);
    const uint8_t en0[] = { LP5012_DEV_CFG0, LP5012_CHIP_EN };
    ESP_RETURN_ON_ERROR(wr(en0, sizeof(en0)), TAG, "chip en");
    /* Auto-increment from DEV_CFG1: CFG1, LED_CFG0 (no banks), then skip. */
    const uint8_t cfg[] = { LP5012_DEV_CFG1, LP5012_CFG1_VALUE, 0x00 };
    ESP_RETURN_ON_ERROR(wr(cfg, sizeof(cfg)), TAG, "config");
    /* Module brightness full; colour registers carry the level. */
    const uint8_t brt[] = { LP5012_LED0_BRT, 0xFF, 0xFF, 0xFF, 0xFF };
    return wr(brt, sizeof(brt));
}

static void hw_set(uint8_t idx, rgb_t c)
{
    /* Module n = OUT(3n) R, OUT(3n+1) G, OUT(3n+2) B (rev A routing). */
    const uint8_t buf[] = { (uint8_t)(LP5012_OUT0_CLR + 3 * idx), c.r, c.g, c.b };

    if (wr(buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "LP5012 write failed");
    }
}
#endif

/* ------------------------------------------------------------ common */

static void dm_listener(const dm_event_t *ev, void *ctx)
{
    if (ev->type != DM_EV_INDICATOR || ev->idx >= CONFIG_INDICATOR_COUNT) {
        return;
    }
    hw_set(ev->idx, colour(ev->indicator));
}

esp_err_t indicators_init(void)
{
    if (CONFIG_INDICATOR_COUNT == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(hw_init(), TAG, "driver init");
    for (uint8_t i = 0; i < CONFIG_INDICATOR_COUNT; i++) {
        hw_set(i, colour(false));
    }
    ESP_RETURN_ON_ERROR(dm_register_listener(dm_listener, NULL), TAG, "listener");
#if CONFIG_INDICATOR_DRIVER_LP5012
    ESP_LOGI(TAG, "%d indicator LED(s) via LP5012 @ 0x%02x", CONFIG_INDICATOR_COUNT, CONFIG_LP5012_I2C_ADDR);
#else
    ESP_LOGI(TAG, "%d indicator LED(s) via WS2812 on GPIO%d", CONFIG_INDICATOR_COUNT, CONFIG_INDICATOR_GPIO);
#endif
    return ESP_OK;
}
