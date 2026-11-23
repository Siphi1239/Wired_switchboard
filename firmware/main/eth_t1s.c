#include "eth_t1s.h"
#include "app_settings.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_lan865x.h"
#include "esp_eth_phy_lan865x.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"

static const char *TAG = "t1s";

#define EVT_IPV4  BIT0

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_events;
static uint8_t s_mac[6];
static char s_dev_id[13];
static bool s_link_up;

const char *eth_t1s_device_id(void)
{
    return s_dev_id;
}

static void eth_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "T1S link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        ESP_LOGW(TAG, "T1S link down");
        break;
    default:
        break;
    }
}

static void ip_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IPv4 " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        xEventGroupSetBits(s_events, EVT_IPV4);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "IPv4 lost");
        xEventGroupClearBits(s_events, EVT_IPV4);
    }
}

esp_err_t eth_t1s_preinit(void)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    return s_events ? ESP_OK : ESP_ERR_NO_MEM;
}

bool eth_t1s_has_ipv4(void)
{
    return s_events && (xEventGroupGetBits(s_events) & EVT_IPV4);
}

bool eth_t1s_wait_ipv4(TickType_t timeout)
{
    return xEventGroupWaitBits(s_events, EVT_IPV4, pdFALSE, pdTRUE, timeout) & EVT_IPV4;
}

esp_err_t eth_t1s_set_plca(uint8_t node_id, uint8_t node_count)
{
    ESP_RETURN_ON_FALSE(s_eth, ESP_ERR_INVALID_STATE, TAG, "driver not installed");

    bool en = (node_id != 255);
    uint8_t tot = CONFIG_T1S_PLCA_TO_TIMER;
    uint8_t burst_cnt = CONFIG_T1S_PLCA_BURST_COUNT;
    uint8_t burst_tmr = CONFIG_T1S_PLCA_BURST_TIMER;

    if (en) {
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_PLCA_ID, &node_id), TAG, "PLCA id");
        /* Node count is only evaluated by the coordinator, harmless elsewhere. */
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_PLCA_NCNT, &node_count), TAG, "PLCA ncnt");
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_PLCA_TOT, &tot), TAG, "PLCA tot");
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_MAX_BURST_COUNT, &burst_cnt), TAG, "burst cnt");
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_BURST_TIMER, &burst_tmr), TAG, "burst tmr");
    }
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_S_EN_PLCA, &en), TAG, "PLCA enable");

    if (en) {
        ESP_LOGI(TAG, "PLCA node %u of %u%s", node_id, node_count, node_id == 0 ? " (coordinator)" : "");
    } else {
        ESP_LOGI(TAG, "PLCA disabled, CSMA/CD");
    }
    return ESP_OK;
}

esp_err_t eth_t1s_get_plca(eth_t1s_plca_t *out)
{
    ESP_RETURN_ON_FALSE(s_eth && out, ESP_ERR_INVALID_STATE, TAG, "driver not installed");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_G_EN_PLCA, &out->plca_enabled), TAG, "get en");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_G_PLCA_ID, &out->node_id), TAG, "get id");
    return esp_eth_ioctl(s_eth, LAN86XX_ETH_CMD_G_PLCA_NCNT, &out->node_count);
}

void eth_t1s_print_status(void)
{
    eth_t1s_plca_t plca = { 0 };
    esp_netif_ip_info_t ip = { 0 };
    esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    const char *host = NULL;

    printf("MAC       : %02x:%02x:%02x:%02x:%02x:%02x\n", s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    printf("link      : %s\n", s_link_up ? "up" : "down");
    if (eth_t1s_get_plca(&plca) == ESP_OK) {
        if (plca.plca_enabled) {
            printf("PLCA      : node %u of %u\n", plca.node_id, plca.node_count);
        } else {
            printf("PLCA      : off (CSMA/CD)\n");
        }
    }
    if (s_netif) {
        esp_netif_get_hostname(s_netif, &host);
        printf("hostname  : %s\n", host ? host : "-");
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
            printf("IPv4      : " IPSTR "\n", IP2STR(&ip.ip));
        }
        int n = esp_netif_get_all_ip6(s_netif, ip6);
        for (int i = 0; i < n; i++) {
            printf("IPv6      : " IPV6STR "\n", IPV62STR(ip6[i]));
        }
    }
}

/*
 * Hook called from chip::DeviceLayer::NetworkCommissioning::ESPEthernetDriver::Init()
 * (Matter task, during esp_matter::start()). Install driver only - no attach/start.
 */
esp_err_t esp_matter_app_ethernet_init(esp_netif_t *eth_netif, esp_eth_handle_t *out_handle)
{
    const app_config_t *cfg = app_cfg();
    esp_err_t err;

    s_netif = eth_netif;
    ESP_RETURN_ON_ERROR(eth_t1s_preinit(), TAG, "preinit");

    /* The LAN865x driver is interrupt driven. The ISR service may already be
     * installed by another driver: ESP_ERR_INVALID_STATE is fine. */
    err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");

    const spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_T1S_GPIO_MISO,
        .mosi_io_num = CONFIG_T1S_GPIO_MOSI,
        .sclk_io_num = CONFIG_T1S_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(CONFIG_T1S_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_T1S_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_T1S_GPIO_CS,
    };
    eth_lan865x_config_t lan_cfg = ETH_LAN865X_DEFAULT_CONFIG(CONFIG_T1S_SPI_HOST, &devcfg);
    lan_cfg.int_gpio_num = CONFIG_T1S_GPIO_INT;
    lan_cfg.poll_period_ms = 0;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = CONFIG_T1S_GPIO_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_lan865x(&lan_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan865x(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "LAN865x MAC/PHY create failed - check SPI wiring");

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &s_eth), TAG, "driver install");

    /* LAN865x has no burned-in MAC: use the ESP32's factory Ethernet MAC
     * (Espressif OUI, globally unique). */
    ESP_RETURN_ON_ERROR(esp_read_mac(s_mac, ESP_MAC_ETH), TAG, "read mac");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, s_mac), TAG, "set mac");
    snprintf(s_dev_id, sizeof(s_dev_id), "%02x%02x%02x%02x%02x%02x", s_mac[0], s_mac[1], s_mac[2], s_mac[3],
             s_mac[4], s_mac[5]);

    err = eth_t1s_set_plca(cfg->plca_node_id, cfg->plca_node_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PLCA config failed (%s), continuing with CSMA/CD", esp_err_to_name(err));
    }

    /* "<name>-<last 3 MAC bytes>" -> DHCP option 12 and mDNS */
    static char host[48];
    snprintf(host, sizeof(host), "%s-%s", cfg->name, &s_dev_id[6]);
    esp_netif_set_hostname(eth_netif, host);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_cb, NULL));

    ESP_LOGI(TAG, "LAN865x ready, MAC %s, hostname %s", s_dev_id, host);
    *out_handle = s_eth;
    return ESP_OK;
}
