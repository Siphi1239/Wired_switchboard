/*
 * 10BASE-T1S via Microchip LAN8650/1 MAC-PHY on SPI.
 *
 * The Matter ESP32 platform creates the "ETH_DEF" netif and calls
 * esp_matter_app_ethernet_init() (hook added by
 * patches/0001-esp32-ethernet-app-init-hook.patch). We install the
 * LAN865x driver there; Matter attaches the netif and starts the driver.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create sync objects. Call early in app_main, before any task waits on IPv4. */
esp_err_t eth_t1s_preinit(void);

typedef struct {
    bool plca_enabled;
    uint8_t node_id;
    uint8_t node_count;
} eth_t1s_plca_t;

/* Apply PLCA settings (also at runtime). node_id 255 = PLCA off (CSMA/CD). */
esp_err_t eth_t1s_set_plca(uint8_t node_id, uint8_t node_count);
esp_err_t eth_t1s_get_plca(eth_t1s_plca_t *out);

bool eth_t1s_has_ipv4(void);
/* Wait until an IPv4 address was obtained (DHCP). */
bool eth_t1s_wait_ipv4(TickType_t timeout);

/* 12 hex chars from the factory Ethernet MAC, used as unique id. */
const char *eth_t1s_device_id(void);

/* Print link, addresses and PLCA status to the console. */
void eth_t1s_print_status(void);

#ifdef __cplusplus
}
#endif
