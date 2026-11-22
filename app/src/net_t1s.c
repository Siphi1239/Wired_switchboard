/* SPDX-License-Identifier: Apache-2.0 */
#include "net_t1s.h"
#include "app_settings.h"
#include "status_led.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

LOG_MODULE_REGISTER(t1s, CONFIG_APP_LOG_LEVEL);

#define L4_EVENTS (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static struct net_if *iface;
static struct net_mgmt_event_callback l4_cb;
static K_EVENT_DEFINE(net_events);
#define EVT_ONLINE BIT(0)

static char device_id[13]; /* 6 bytes hex */
static uint8_t mac[6];

const char *net_t1s_device_id(void)
{
	return device_id;
}

static void make_identity(void)
{
	uint8_t id[8] = {0};
	ssize_t len = hwinfo_get_device_id(id, sizeof(id));

	if (len <= 0) {
		LOG_WRN("hwinfo unavailable, using fallback id");
		memcpy(id, "\x00\x00\xde\xad\xbe\xef", 6);
	}

	/*
	 * Locally administered, unicast MAC from the low 5 bytes of the
	 * FICR DEVICEID. For a product, buy an OUI block or use a pre-
	 * programmed EUI-48 EEPROM (e.g. 24AA02E48) instead.
	 */
	mac[0] = 0x02;
	memcpy(&mac[1], &id[len > 5 ? len - 5 : 0], 5);
	snprintk(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
		 mac[3], mac[4], mac[5]);
}

/*
 * Both NET_REQUEST_ETHERNET_SET_MAC_ADDRESS and ..._SET_T1S_PARAM are refused
 * (-EACCES) by the Zephyr ethernet mgmt layer while the interface is up.
 */
static bool iface_down(void)
{
	bool was_up = net_if_is_admin_up(iface);

	if (was_up) {
		net_if_down(iface);
	}
	return was_up;
}

static void iface_restore(bool was_up)
{
	if (was_up) {
		net_if_up(iface);
	}
}

static int set_mac(void)
{
	struct ethernet_req_params p = {0};

	memcpy(p.mac_address.addr, mac, sizeof(mac));
	return net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &p, sizeof(p));
}

static int set_plca(uint8_t node_id, uint8_t node_count)
{
	struct ethernet_req_params p = {0};

	p.t1s_param.type = ETHERNET_T1S_PARAM_TYPE_PLCA_CONFIG;
	p.t1s_param.plca.enable = (node_id != 255);
	p.t1s_param.plca.node_id = node_id;
	p.t1s_param.plca.node_count = node_count;
	p.t1s_param.plca.burst_count = CONFIG_APP_PLCA_BURST_COUNT;
	p.t1s_param.plca.burst_timer = CONFIG_APP_PLCA_BURST_TIMER;
	p.t1s_param.plca.to_timer = CONFIG_APP_PLCA_TO_TIMER;

	int err = net_mgmt(NET_REQUEST_ETHERNET_SET_T1S_PARAM, iface, &p, sizeof(p));

	if (err) {
		LOG_ERR("PLCA config failed (%d)", err);
	} else if (p.t1s_param.plca.enable) {
		LOG_INF("PLCA node %u/%u%s", node_id, node_count,
			node_id == 0 ? " (coordinator)" : "");
	} else {
		LOG_INF("PLCA disabled (CSMA/CD)");
	}
	return err;
}

int net_t1s_apply_plca(uint8_t node_id, uint8_t node_count)
{
	if (!iface) {
		return -ENODEV;
	}
	/* Takes the link down briefly; MQTT/Matter reconnect on L4 up. */
	bool was_up = iface_down();
	int err = set_plca(node_id, node_count);

	iface_restore(was_up);
	return err;
}

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *i)
{
	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("network online");
		k_event_post(&net_events, EVT_ONLINE);
		status_led_set(STATUS_NETWORK_UP);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_WRN("network offline");
		k_event_clear(&net_events, EVT_ONLINE);
		status_led_set(STATUS_NO_NETWORK);
	}
}

bool net_t1s_is_online(void)
{
	return k_event_test(&net_events, EVT_ONLINE) != 0;
}

int net_t1s_wait_online(k_timeout_t timeout)
{
	return k_event_wait(&net_events, EVT_ONLINE, false, timeout) ? 0 : -EAGAIN;
}

int net_t1s_init(void)
{
	const struct app_config *cfg = app_cfg();
	char host[CONFIG_NET_HOSTNAME_MAX_LEN];
	int err;

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	if (!iface) {
		LOG_ERR("no Ethernet interface - LAN865x not probed? Check SPI/IRQ/RESET wiring");
		return -ENODEV;
	}

	make_identity();

	bool was_up = iface_down();

	err = set_mac();
	if (err) {
		LOG_WRN("MAC override failed (%d), keeping DT address", err);
	}
	err = set_plca(cfg->plca_node_id, cfg->plca_node_count);
	iface_restore(was_up);
	if (err) {
		return err;
	}

	/* "<name>-<last 3 MAC bytes>.local" via the mDNS responder. */
	snprintk(host, sizeof(host), "%s-%s", cfg->name, &device_id[6]);
	net_hostname_set(host, strlen(host));
	LOG_INF("hostname %s.local, MAC %02x:%02x:%02x:%02x:%02x:%02x", host, mac[0], mac[1],
		mac[2], mac[3], mac[4], mac[5]);

	net_mgmt_init_event_callback(&l4_cb, l4_handler, L4_EVENTS);
	net_mgmt_add_event_callback(&l4_cb);
	status_led_set(STATUS_NO_NETWORK);

	/* Fire L4 events for the current state, in case we are already up. */
	conn_mgr_mon_resend_status();

	net_dhcpv4_start(iface);
	return 0;
}
