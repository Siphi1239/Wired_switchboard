/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MQTT front-end with Home Assistant discovery.
 *
 * Topics (base = <prefix>/<name>):
 *   <base>/status          "online" / "offline"   retained, LWT
 *   <base>/ch/<n>/state    "ON" / "OFF"           retained
 *   <base>/ch/<n>/set      "ON" / "OFF" / "TOGGLE" (subscribed, QoS 1)
 *   <ha>/switch/<name>/ch<n>/config               retained discovery JSON
 *
 * The Zephyr MQTT client is not thread-safe, so everything runs on one
 * thread. The device-model listener only sets bits in an atomic mask that
 * the thread drains within one poll period.
 */
#include "mqtt_app.h"
#include <app_version.h>
#include "app_settings.h"
#include "device_model.h"
#include "net_t1s.h"
#include "status_led.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(mqtt_app, CONFIG_APP_LOG_LEVEL);

#define POLL_PERIOD_MS  100
#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  60000
#define TOPIC_MAX       96
#define PAYLOAD_MAX     512

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[256];
static uint8_t tx_buf[PAYLOAD_MAX + 128];
static char payload[PAYLOAD_MAX];
static char base[TOPIC_MAX - 24];
static char client_id[40];

static char will_topic_str[TOPIC_MAX];
static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg = {.utf8 = (const uint8_t *)"offline", .size = 7};

static atomic_t dirty;        /* channels whose state must be published */
static atomic_t reconnect_req;
static volatile bool connected;

/* ------------------------------------------------------------------ helpers */

static int publish(const char *topic, const char *data, size_t len, bool retain)
{
	struct mqtt_publish_param p = {
		.message.topic.topic.utf8 = (const uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.message.payload.data = (uint8_t *)data,
		.message.payload.len = len,
		.message_id = sys_rand16_get(),
		.retain_flag = retain,
	};

	return mqtt_publish(&client, &p);
}

static int publish_state(size_t ch)
{
	char topic[TOPIC_MAX];
	const char *v = dm_get(ch) ? "ON" : "OFF";

	snprintk(topic, sizeof(topic), "%s/ch/%u/state", base, (unsigned)ch);
	return publish(topic, v, strlen(v), true);
}

#if defined(CONFIG_APP_MQTT_HA_DISCOVERY)
static int publish_discovery(size_t ch)
{
	const struct app_config *cfg = app_cfg();
	const char *id = net_t1s_device_id();
	char topic[TOPIC_MAX + 32];
	int n;

	snprintk(topic, sizeof(topic), CONFIG_APP_MQTT_HA_DISCOVERY_PREFIX "/switch/%s/ch%u/config",
		 cfg->name, (unsigned)ch);

	n = snprintk(payload, sizeof(payload),
		     "{\"name\":\"Channel %u\","
		     "\"uniq_id\":\"%s_ch%u\","
		     "\"stat_t\":\"%s/ch/%u/state\","
		     "\"cmd_t\":\"%s/ch/%u/set\","
		     "\"avty_t\":\"%s/status\","
		     "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
		     "\"mf\":\"DIY\",\"mdl\":\"Wired Switchboard (10BASE-T1S)\","
		     "\"sw\":\"" APP_VERSION_STRING "\"}}",
		     (unsigned)ch + 1, id, (unsigned)ch, base, (unsigned)ch, base, (unsigned)ch,
		     base, id, cfg->name);
	if (n < 0 || (size_t)n >= sizeof(payload)) {
		return -ENOMEM;
	}
	return publish(topic, payload, n, true);
}
#endif

static void on_connected(void)
{
	struct mqtt_topic sub_topic;
	char sub[TOPIC_MAX];
	int err;

	snprintk(sub, sizeof(sub), "%s/ch/+/set", base);
	sub_topic = (struct mqtt_topic){
		.topic = {.utf8 = (const uint8_t *)sub, .size = strlen(sub)},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	const struct mqtt_subscription_list subs = {
		.list = &sub_topic,
		.list_count = 1,
		.message_id = sys_rand16_get(),
	};

	err = mqtt_subscribe(&client, &subs);
	if (err) {
		LOG_ERR("subscribe failed (%d)", err);
	}

	publish(will_topic_str, "online", 6, true);
#if defined(CONFIG_APP_MQTT_HA_DISCOVERY)
	for (size_t ch = 0; ch < dm_channel_count(); ch++) {
		publish_discovery(ch);
	}
#endif
	/* Publish everything once; the broker keeps it retained. */
	atomic_set(&dirty, BIT_MASK(dm_channel_count()));
}

/* "<base>/ch/<n>/set" -> n, or -1 */
static int parse_set_topic(const uint8_t *t, size_t len)
{
	size_t bl = strlen(base);
	static const char pfx[] = "/ch/";
	static const char sfx[] = "/set";
	unsigned int ch = 0;
	size_t i;

	if (len <= bl + sizeof(pfx) - 1 + sizeof(sfx) - 1 || memcmp(t, base, bl) ||
	    memcmp(t + bl, pfx, sizeof(pfx) - 1) ||
	    memcmp(t + len - (sizeof(sfx) - 1), sfx, sizeof(sfx) - 1)) {
		return -1;
	}
	for (i = bl + sizeof(pfx) - 1; i < len - (sizeof(sfx) - 1); i++) {
		if (t[i] < '0' || t[i] > '9' || ch > 99) {
			return -1;
		}
		ch = ch * 10 + (t[i] - '0');
	}
	return ch;
}

static void handle_publish(const struct mqtt_publish_param *pub)
{
	char val[8] = {0};
	size_t len = pub->message.payload.len;
	int ch;

	/* Always drain the payload, otherwise the RX stream gets out of sync. */
	if (len >= sizeof(val)) {
		uint8_t junk[32];

		while (len) {
			int r = mqtt_read_publish_payload_blocking(&client, junk,
								   MIN(len, sizeof(junk)));
			if (r <= 0) {
				break;
			}
			len -= r;
		}
		LOG_WRN("oversized command payload dropped");
		goto ack;
	}
	if (mqtt_readall_publish_payload(&client, (uint8_t *)val, len) < 0) {
		return;
	}

	ch = parse_set_topic(pub->message.topic.topic.utf8, pub->message.topic.topic.size);
	if (ch < 0 || (size_t)ch >= dm_channel_count()) {
		LOG_WRN("command for unknown channel");
	} else if (!strcmp(val, "ON") || !strcmp(val, "1") || !strcmp(val, "true")) {
		dm_set(ch, true, DM_SRC_MQTT);
	} else if (!strcmp(val, "OFF") || !strcmp(val, "0") || !strcmp(val, "false")) {
		dm_set(ch, false, DM_SRC_MQTT);
	} else if (!strcmp(val, "TOGGLE")) {
		dm_toggle(ch, DM_SRC_MQTT);
	} else {
		LOG_WRN("ch%d: unknown command '%s'", ch, val);
	}

ack:
	if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = {.message_id = pub->message_id};

		mqtt_publish_qos1_ack(&client, &ack);
	}
}

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result) {
			LOG_ERR("CONNACK refused (%d)", evt->result);
			break;
		}
		LOG_INF("connected to broker");
		connected = true;
		status_led_set(STATUS_ONLINE);
		on_connected();
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("disconnected (%d)", evt->result);
		connected = false;
		status_led_set(net_t1s_is_online() ? STATUS_NETWORK_UP : STATUS_NO_NETWORK);
		break;
	case MQTT_EVT_PUBLISH:
		handle_publish(&evt->param.publish);
		break;
	case MQTT_EVT_SUBACK:
		LOG_DBG("SUBACK");
		break;
	default:
		break;
	}
}

static void dm_listener(size_t ch, bool on, enum dm_source src)
{
	ARG_UNUSED(on);
	ARG_UNUSED(src);
	atomic_set_bit(&dirty, ch);
}

/* ------------------------------------------------------------------ session */

static int resolve_broker(void)
{
	const struct app_config *cfg = app_cfg();
	struct zsock_addrinfo hints = {.ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *res;
	char port[6];
	int err;

	snprintk(port, sizeof(port), "%u", cfg->mqtt_port);
	err = zsock_getaddrinfo(cfg->mqtt_host, port, &hints, &res);
	if (err) {
		LOG_WRN("cannot resolve %s (%d)", cfg->mqtt_host, err);
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, MIN(res->ai_addrlen, sizeof(broker)));
	zsock_freeaddrinfo(res);
	return 0;
}

static int session_open(void)
{
	const struct app_config *cfg = app_cfg();
	int err;

	snprintk(base, sizeof(base), CONFIG_APP_MQTT_TOPIC_PREFIX "/%s", cfg->name);
	snprintk(will_topic_str, sizeof(will_topic_str), "%s/status", base);
	snprintk(client_id, sizeof(client_id), "%s-%s", cfg->name, net_t1s_device_id());

	err = resolve_broker();
	if (err) {
		return err;
	}

	mqtt_client_init(&client);
	will_topic = (struct mqtt_topic){
		.topic = {.utf8 = (const uint8_t *)will_topic_str, .size = strlen(will_topic_str)},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (const uint8_t *)client_id;
	client.client_id.size = strlen(client_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.keepalive = CONFIG_APP_MQTT_KEEPALIVE_S;
	client.clean_session = 1;
	client.will_topic = &will_topic;
	client.will_message = &will_msg;
	client.will_retain = 1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	/* TODO(security): MQTT_TRANSPORT_SECURE + TLS credentials, see docs. */
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	LOG_INF("connecting to %s:%u as %s", cfg->mqtt_host, cfg->mqtt_port, client_id);
	return mqtt_connect(&client);
}

static void session_run(void)
{
	struct zsock_pollfd fds = {.fd = client.transport.tcp.sock, .events = ZSOCK_POLLIN};
	int64_t connect_deadline = k_uptime_get() + 10000;

	while (!atomic_cas(&reconnect_req, 1, 0)) {
		int keep = mqtt_keepalive_time_left(&client);
		int rc = zsock_poll(&fds, 1, MIN(keep, POLL_PERIOD_MS));

		if (rc < 0) {
			LOG_ERR("poll error %d", errno);
			break;
		}
		if (rc > 0) {
			if (fds.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR | ZSOCK_POLLNVAL)) {
				LOG_WRN("socket closed");
				break;
			}
			if ((fds.revents & ZSOCK_POLLIN) && mqtt_input(&client) < 0) {
				break;
			}
		}
		rc = mqtt_live(&client);
		if (rc < 0 && rc != -EAGAIN) {
			LOG_WRN("keep-alive failed (%d)", rc);
			break;
		}
		if (!connected) {
			if (k_uptime_get() > connect_deadline) {
				LOG_WRN("CONNACK timeout");
				break;
			}
			continue;
		}

		atomic_val_t bits = atomic_clear(&dirty);

		for (size_t ch = 0; bits && ch < dm_channel_count(); ch++) {
			if ((bits & BIT(ch)) && publish_state(ch) < 0) {
				atomic_or(&dirty, BIT(ch)); /* retry next round */
			}
		}
	}

	if (connected) {
		publish(will_topic_str, "offline", 7, true);
		mqtt_disconnect(&client, NULL);
	} else {
		mqtt_abort(&client);
	}
	connected = false;
}

static void mqtt_thread(void *a, void *b, void *c)
{
	uint32_t backoff = BACKOFF_MIN_MS;

	for (;;) {
		net_t1s_wait_online(K_FOREVER);

		if (session_open() == 0) {
			backoff = BACKOFF_MIN_MS;
			session_run();
		}

		/* Exponential backoff with jitter: avoids all nodes on a T1S
		 * segment hammering the broker in lock-step after a bus reset. */
		uint32_t delay = backoff / 2 + sys_rand32_get() % (backoff / 2 + 1);

		LOG_INF("reconnect in %u ms", delay);
		k_sleep(K_MSEC(delay));
		backoff = MIN(backoff * 2, BACKOFF_MAX_MS);
	}
}

K_THREAD_STACK_DEFINE(mqtt_stack, CONFIG_APP_MQTT_THREAD_STACK_SIZE);
static struct k_thread mqtt_tid;

int mqtt_app_init(void)
{
	int err = dm_register_listener(dm_listener);

	if (err) {
		return err;
	}
	k_thread_create(&mqtt_tid, mqtt_stack, K_THREAD_STACK_SIZEOF(mqtt_stack), mqtt_thread,
			NULL, NULL, NULL, CONFIG_APP_MQTT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&mqtt_tid, "mqtt");
	return 0;
}

void mqtt_app_reconnect(void)
{
	atomic_set(&reconnect_req, 1);
}

bool mqtt_app_is_connected(void)
{
	return connected;
}
