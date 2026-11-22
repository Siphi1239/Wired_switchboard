/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Device model: the single owner of the switchboard state.
 *
 * Protocol front-ends (MQTT, Matter) and the local I/O never talk to each
 * other directly. They call dm_set()/dm_toggle() and get notified through
 * listeners. This keeps MQTT and Matter consistent without either protocol
 * "owning" the relay state.
 */
#ifndef DEVICE_MODEL_H_
#define DEVICE_MODEL_H_

#include <stdbool.h>
#include <stddef.h>

#define DM_MAX_CHANNELS 8

enum dm_source {
	DM_SRC_BOOT,   /* restored from NVS */
	DM_SRC_LOCAL,  /* physical button / wall switch */
	DM_SRC_MQTT,
	DM_SRC_MATTER,
	DM_SRC_SHELL,
};

/* Called from the system work queue. Must not block. */
typedef void (*dm_listener_t)(size_t ch, bool on, enum dm_source src);

int dm_init(size_t channel_count);
size_t dm_channel_count(void);
bool dm_get(size_t ch);
int dm_set(size_t ch, bool on, enum dm_source src);
int dm_toggle(size_t ch, enum dm_source src);
int dm_register_listener(dm_listener_t cb);
const char *dm_source_str(enum dm_source src);

#endif /* DEVICE_MODEL_H_ */
