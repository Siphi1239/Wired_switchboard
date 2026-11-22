/* SPDX-License-Identifier: Apache-2.0 */
#include "status_led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER_NODE, status_led_gpios)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(USER_NODE, status_led_gpios);
static enum status_led_mode mode = STATUS_BOOT;
static uint32_t tick;

static void led_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_fn);

/* 100 ms tick; pattern = bitmask over a 20-tick (2 s) period. */
static bool pattern_on(enum status_led_mode m, uint32_t t)
{
	switch (m) {
	case STATUS_BOOT:       return true;
	case STATUS_NO_NETWORK: return (t % 2) == 0;
	case STATUS_NETWORK_UP: return (t % 20) < 10;
	case STATUS_ONLINE:     return (t % 50) == 0;
	case STATUS_ERROR:      return (t % 20) == 0 || (t % 20) == 2;
	}
	return false;
}

static void led_work_fn(struct k_work *w)
{
	gpio_pin_set_dt(&led, pattern_on(mode, tick++));
	k_work_schedule(&led_work, K_MSEC(100));
}

int status_led_init(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	if (!err) {
		k_work_schedule(&led_work, K_MSEC(100));
	}
	return err;
}

void status_led_set(enum status_led_mode m)
{
	mode = m;
	tick = 0;
}
#else
int status_led_init(void)
{
	return 0;
}
void status_led_set(enum status_led_mode m)
{
	ARG_UNUSED(m);
}
#endif
