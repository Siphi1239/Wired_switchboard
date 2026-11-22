/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Local I/O: relay outputs and wall-switch / push-button inputs, taken from
 * the `zephyr,user` devicetree node:
 *
 *   relay-gpios        - one per channel (outputs)
 *   button-gpios       - one per channel (inputs, toggle on press)
 *   reset-button-gpios - optional, long press (>= 5 s) = factory reset
 */
#include "io_board.h"
#include "device_model.h"
#include "app_settings.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(io, CONFIG_APP_LOG_LEVEL);

#define USER_NODE DT_PATH(zephyr_user)

#define N_RELAYS  DT_PROP_LEN(USER_NODE, relay_gpios)
#define N_BUTTONS DT_PROP_LEN_OR(USER_NODE, button_gpios, 0)

BUILD_ASSERT(N_RELAYS <= DM_MAX_CHANNELS, "too many relay channels");
BUILD_ASSERT(N_BUTTONS == 0 || N_BUTTONS == N_RELAYS,
	     "button-gpios must match relay-gpios 1:1 (or be absent)");

#define GPIO_SPEC_ELEM(node, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node, prop, idx),

static const struct gpio_dt_spec relays[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, relay_gpios, GPIO_SPEC_ELEM)
};

#if N_BUTTONS > 0
static const struct gpio_dt_spec buttons[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, button_gpios, GPIO_SPEC_ELEM)
};
static struct gpio_callback button_cb[N_BUTTONS];
static struct k_work_delayable debounce_work[N_BUTTONS];
#endif

#define DEBOUNCE_MS 30

#if DT_NODE_HAS_PROP(USER_NODE, reset_button_gpios)
#define HAS_RESET_BUTTON 1
#define FACTORY_RESET_HOLD_MS 5000
static const struct gpio_dt_spec reset_btn = GPIO_DT_SPEC_GET(USER_NODE, reset_button_gpios);
static struct gpio_callback reset_cb;
static struct k_work_delayable reset_work;
#else
#define HAS_RESET_BUTTON 0
#endif

size_t io_board_channel_count(void)
{
	return N_RELAYS;
}

static void relay_listener(size_t ch, bool on, enum dm_source src)
{
	ARG_UNUSED(src);
	if (ch < N_RELAYS) {
		gpio_pin_set_dt(&relays[ch], on);
	}
}

#if N_BUTTONS > 0
static void debounce_fn(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	size_t ch = dw - debounce_work;

	/* Toggle on a stable press. For latching wall switches (not push
	 * buttons) you would instead toggle on every stable edge. */
	if (gpio_pin_get_dt(&buttons[ch]) == 1) {
		dm_toggle(ch, DM_SRC_LOCAL);
	}
}

static void button_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	size_t ch = cb - button_cb;

	k_work_reschedule(&debounce_work[ch], K_MSEC(DEBOUNCE_MS));
}
#endif

#if HAS_RESET_BUTTON
static void reset_fn(struct k_work *work)
{
	if (gpio_pin_get_dt(&reset_btn) == 1) {
		LOG_WRN("factory reset requested");
		app_settings_factory_reset();
		sys_reboot(SYS_REBOOT_COLD);
	}
}

static void reset_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	if (gpio_pin_get_dt(&reset_btn) == 1) {
		k_work_reschedule(&reset_work, K_MSEC(FACTORY_RESET_HOLD_MS));
	} else {
		k_work_cancel_delayable(&reset_work);
	}
}
#endif

static int setup_input(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
		       gpio_callback_handler_t handler, gpio_flags_t edge)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (err) {
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(spec, edge);
	if (err) {
		return err;
	}
	gpio_init_callback(cb, handler, BIT(spec->pin));
	return gpio_add_callback_dt(spec, cb);
}

int io_board_init(void)
{
	int err;

	for (size_t i = 0; i < N_RELAYS; i++) {
		if (!gpio_is_ready_dt(&relays[i])) {
			return -ENODEV;
		}
		/* Safe state until the device model has restored/decided. */
		err = gpio_pin_configure_dt(&relays[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	err = dm_register_listener(relay_listener);
	if (err) {
		return err;
	}

#if N_BUTTONS > 0
	for (size_t i = 0; i < N_BUTTONS; i++) {
		k_work_init_delayable(&debounce_work[i], debounce_fn);
		err = setup_input(&buttons[i], &button_cb[i], button_isr, GPIO_INT_EDGE_BOTH);
		if (err) {
			LOG_ERR("button %u setup failed (%d)", (unsigned)i, err);
			return err;
		}
	}
#endif

#if HAS_RESET_BUTTON
	k_work_init_delayable(&reset_work, reset_fn);
	err = setup_input(&reset_btn, &reset_cb, reset_isr, GPIO_INT_EDGE_BOTH);
	if (err) {
		LOG_WRN("reset button setup failed (%d)", err);
	}
#endif

	LOG_INF("%u relay channel(s), %u button(s)", N_RELAYS, N_BUTTONS);
	return 0;
}
