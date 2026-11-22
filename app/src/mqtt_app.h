/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MQTT_APP_H_
#define MQTT_APP_H_

#include <stdbool.h>

/* Register with the device model and start the MQTT thread. */
int mqtt_app_init(void);

/* Drop the current session and reconnect (e.g. after broker change). */
void mqtt_app_reconnect(void);

bool mqtt_app_is_connected(void);

#endif
