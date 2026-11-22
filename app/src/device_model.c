/* SPDX-License-Identifier: Apache-2.0 */
#include "device_model.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(dm, CONFIG_APP_LOG_LEVEL);

#define MAX_LISTENERS 4

static size_t n_channels;
static atomic_t state;          /* bit n = channel n on */
static atomic_t pending_notify; /* bits to notify */
static enum dm_source last_src[DM_MAX_CHANNELS];
static dm_listener_t listeners[MAX_LISTENERS];
static size_t n_listeners;
static K_MUTEX_DEFINE(lock);

static void notify_work_fn(struct k_work *w);
static K_WORK_DEFINE(notify_work, notify_work_fn);

#if defined(CONFIG_APP_STATE_RESTORE)
static void save_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);
static uint32_t restored_state;
static bool restored_valid;

static int dm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "state") == 0 && len == sizeof(restored_state)) {
		if (read_cb(cb_arg, &restored_state, sizeof(restored_state)) ==
		    sizeof(restored_state)) {
			restored_valid = true;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(dm, "dm", NULL, dm_settings_set, NULL, NULL);

static void save_work_fn(struct k_work *w)
{
	uint32_t s = (uint32_t)atomic_get(&state);
	int err = settings_save_one("dm/state", &s, sizeof(s));

	if (err) {
		LOG_WRN("persist state failed (%d)", err);
	}
}
#endif

static void notify_work_fn(struct k_work *w)
{
	atomic_val_t bits = atomic_clear(&pending_notify);
	atomic_val_t s = atomic_get(&state);

	for (size_t ch = 0; ch < n_channels; ch++) {
		if (!(bits & BIT(ch))) {
			continue;
		}
		for (size_t i = 0; i < n_listeners; i++) {
			listeners[i](ch, (s & BIT(ch)) != 0, last_src[ch]);
		}
	}
}

int dm_init(size_t channel_count)
{
	if (channel_count == 0 || channel_count > DM_MAX_CHANNELS) {
		return -EINVAL;
	}
	n_channels = channel_count;
	atomic_clear(&state);

#if defined(CONFIG_APP_STATE_RESTORE)
	/* settings_load() has run in main() before dm_init(). */
	if (restored_valid) {
		LOG_INF("restoring channel state 0x%02x", restored_state);
		for (size_t ch = 0; ch < n_channels; ch++) {
			dm_set(ch, restored_state & BIT(ch), DM_SRC_BOOT);
		}
	}
#endif
	return 0;
}

size_t dm_channel_count(void)
{
	return n_channels;
}

bool dm_get(size_t ch)
{
	return ch < n_channels && (atomic_get(&state) & BIT(ch));
}

int dm_set(size_t ch, bool on, enum dm_source src)
{
	if (ch >= n_channels) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	bool was = atomic_test_bit(&state, ch);

	if (on) {
		atomic_set_bit(&state, ch);
	} else {
		atomic_clear_bit(&state, ch);
	}
	last_src[ch] = src;
	k_mutex_unlock(&lock);

	/*
	 * Always notify on boot (front-ends need the initial value) and on
	 * change. A "set" to the same value from a remote is still echoed so
	 * that a controller with a stale view converges.
	 */
	if (was != on || src == DM_SRC_BOOT || src == DM_SRC_MQTT || src == DM_SRC_MATTER) {
		LOG_INF("ch%u -> %s (%s)", (unsigned)ch, on ? "ON" : "OFF", dm_source_str(src));
		atomic_set_bit(&pending_notify, ch);
		k_work_submit(&notify_work);
#if defined(CONFIG_APP_STATE_RESTORE)
		if (src != DM_SRC_BOOT && was != on) {
			k_work_reschedule(&save_work, K_MSEC(CONFIG_APP_STATE_SAVE_DELAY_MS));
		}
#endif
	}
	return 0;
}

int dm_toggle(size_t ch, enum dm_source src)
{
	if (ch >= n_channels) {
		return -EINVAL;
	}
	return dm_set(ch, !dm_get(ch), src);
}

int dm_register_listener(dm_listener_t cb)
{
	if (n_listeners >= MAX_LISTENERS) {
		return -ENOMEM;
	}
	listeners[n_listeners++] = cb;
	return 0;
}

const char *dm_source_str(enum dm_source src)
{
	switch (src) {
	case DM_SRC_BOOT:   return "boot";
	case DM_SRC_LOCAL:  return "local";
	case DM_SRC_MQTT:   return "mqtt";
	case DM_SRC_MATTER: return "matter";
	case DM_SRC_SHELL:  return "shell";
	}
	return "?";
}
