/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NET_T1S_H_
#define NET_T1S_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/* Bring up the 10BASE-T1S interface: MAC from FICR, PLCA from settings,
 * hostname, DHCPv4 (+ IPv6 SLAAC/link-local, needed for Matter). */
int net_t1s_init(void);

/* Re-apply PLCA parameters at runtime (e.g. after `sb plca`). */
int net_t1s_apply_plca(uint8_t node_id, uint8_t node_count);

/* True once an IPv4 or global/ULA IPv6 address is usable (conn_mgr L4 up). */
bool net_t1s_is_online(void);

/* Block until online or timeout. Returns 0 when online, -EAGAIN on timeout. */
int net_t1s_wait_online(k_timeout_t timeout);

/* Unique per device, derived from FICR; usable for MQTT client id. */
const char *net_t1s_device_id(void);

#endif
