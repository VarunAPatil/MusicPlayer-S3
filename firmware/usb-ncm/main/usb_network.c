#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "lwip/esp_netif_net_stack.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

#include "usb_network.h"

static const char *TAG = "usb_network";
static esp_netif_t *s_usb_netif;

static esp_err_t usb_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle;
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

static void free_received_frame(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t usb_receive(void *buffer, uint16_t len, void *context)
{
    (void)context;
    if (s_usb_netif == NULL) return ESP_OK;

    void *copy = malloc(len);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_usb_netif, copy, len, NULL);
}

esp_err_t usb_network_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");

    const tinyusb_config_t usb_config = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb_config), TAG, "TinyUSB init failed");

    const tinyusb_net_config_t ncm_config = {
        .mac_addr = {0x02, 0x71, 0x19, 0x08, 0x08, 0x01},
        .on_recv_callback = usb_receive,
    };
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &ncm_config),
                        TAG, "USB NCM init failed");

    static const esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 77, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 252),
        .gw.addr = ESP_IP4TOADDR(0, 0, 0, 0),
    };
    const esp_netif_inherent_config_t base = {
        .flags = ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM spectrum receiver",
        .route_prio = 10,
    };
    const esp_netif_driver_ifconfig_t driver = {
        .handle = (void *)1,
        .transmit = usb_transmit,
        .driver_free_rx_buffer = free_received_frame,
    };
    const struct esp_netif_netstack_config stack = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    const esp_netif_config_t config = {
        .base = &base,
        .driver = &driver,
        .stack = &stack,
    };
    s_usb_netif = esp_netif_new(&config);
    if (s_usb_netif == NULL) return ESP_FAIL;

    uint8_t lwip_mac[6] = {0x02, 0x71, 0x19, 0x08, 0x08, 0x02};
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_usb_netif, lwip_mac), TAG, "MAC setup failed");
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    ESP_LOGI(TAG, "USB NCM ready: ESP32 192.168.77.1/30, laptop 192.168.77.2/30");
    return ESP_OK;
}
