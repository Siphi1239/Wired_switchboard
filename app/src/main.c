/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wired Switchboard - nRF52840 + LAN8651 10BASE-T1S, powered over the data line.
 *
 * Init order matters:
 *   1. settings   - provisioning + persisted channel state
 *   2. outputs    - relays to a safe state, register as DM listener
 *   3. front-ends - MQTT (and Matter in phase 2) register as DM listeners
 *   4. dm_init    - restore channel state -> drives relays, notifies front-ends
 *   5. network    - MAC, PLCA, DHCP; front-ends connect once online
 */
#include "app_settings.h"
#include "device_model.h"
#include "io_board.h"
#include "net_t1s.h"
#include "status_led.h"
#if defined(CONFIG_APP_MQTT)
#include "mqtt_app.h"
#endif

#include <app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define CHECK(expr)                                                                        \
	do {                                                                               \
		int _err = (expr);                                                         \
		if (_err) {                                                                \
			LOG_ERR("%s failed (%d)", #expr, _err);                            \
			status_led_set(STATUS_ERROR);                                      \
			return _err;                                                       \
		}                                                                          \
	} while (0)

int main(void)
{
	LOG_INF("Wired Switchboard v" APP_VERSION_EXTENDED_STRING);

	status_led_init();
	CHECK(app_settings_init());
	CHECK(io_board_init());
#if defined(CONFIG_APP_MQTT)
	CHECK(mqtt_app_init());
#endif
	CHECK(dm_init(io_board_channel_count()));
	CHECK(net_t1s_init());

	/* Everything else is event driven (work queue, MQTT thread). */
	return 0;
}
