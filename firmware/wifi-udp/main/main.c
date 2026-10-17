#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

// Change these before building. The host transmitter sends to the DHCP address
// printed in the ESP-IDF monitor after Wi-Fi connects.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define UDP_PORT 4210

#define LED_MATRIX_CLK 18
#define LED_MATRIX_DIN 16
#define LED_MATRIX_LOAD 17
#define MATRIX_INTENSITY 4
#define PACKET_TIMEOUT_MS 200
#define TIMEOUT_DECAY_STEP_MS 50

#define CENTER_MIRROR 1
#define MATRIX_ROTATION 0       // 0, 90, 180, or 270 degrees clockwise
#define MIRROR_X 0
#define MIRROR_Y 0
#define DIGITS_ARE_COLUMNS 1    // Set to 0 for row-wired modules.

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi_spectrum";
static EventGroupHandle_t wifi_events;
static spi_device_handle_t matrix_spi;
static uint8_t heights[8];
static uint8_t last_digits[8];
static int64_t last_packet_ms;
static int64_t last_decay_ms;

static esp_err_t max7219_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    spi_transaction_t transaction = {.length = 16, .tx_buffer = tx};
    return spi_device_polling_transmit(matrix_spi, &transaction);
}

static void matrix_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = LED_MATRIX_DIN,
        .miso_io_num = -1,
        .sclk_io_num = LED_MATRIX_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t device = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = LED_MATRIX_LOAD,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &matrix_spi));
    ESP_ERROR_CHECK(max7219_write(0x0C, 0));
    ESP_ERROR_CHECK(max7219_write(0x0F, 0));
    ESP_ERROR_CHECK(max7219_write(0x09, 0));
    ESP_ERROR_CHECK(max7219_write(0x0B, 7));
    ESP_ERROR_CHECK(max7219_write(0x0A, MATRIX_INTENSITY));
    for (uint8_t digit = 0; digit < 8; ++digit) {
        ESP_ERROR_CHECK(max7219_write(digit + 1, 0));
    }
    ESP_ERROR_CHECK(max7219_write(0x0C, 1));
}

static void map_pixel(uint8_t x, uint8_t y, uint8_t *digit, uint8_t *bit)
{
    if (MIRROR_X) x = 7 - x;
    if (MIRROR_Y) y = 7 - y;
    uint8_t px = x, py = y;
    switch (MATRIX_ROTATION) {
    case 90:  px = 7 - y; py = x; break;
    case 180: px = 7 - x; py = 7 - y; break;
    case 270: px = y; py = 7 - x; break;
    default: break;
    }
    if (DIGITS_ARE_COLUMNS) {
        *digit = px;
        *bit = 7 - py;
    } else {
        *digit = py;
        *bit = 7 - px;
    }
}

static void layout_bars(uint8_t display[8])
{
#if CENTER_MIRROR
    uint8_t paired[4];
    for (uint8_t i = 0; i < 4; ++i) {
        paired[i] = heights[2 * i] > heights[2 * i + 1]
                  ? heights[2 * i] : heights[2 * i + 1];
    }
    display[0] = display[7] = paired[3];
    display[1] = display[6] = paired[2];
    display[2] = display[5] = paired[1];
    display[3] = display[4] = paired[0];
#else
    memcpy(display, heights, sizeof(heights));
#endif
}

static void draw_bars(void)
{
    uint8_t digits[8] = {0};
    uint8_t columns[8];
    layout_bars(columns);
    for (uint8_t x = 0; x < 8; ++x) {
        for (uint8_t y = 8 - columns[x]; y < 8; ++y) {
            uint8_t digit, bit;
            map_pixel(x, y, &digit, &bit);
            digits[digit] |= (uint8_t)(1U << bit);
        }
    }
    for (uint8_t digit = 0; digit < 8; ++digit) {
        if (digits[digit] != last_digits[digit]) {
            ESP_ERROR_CHECK(max7219_write(digit + 1, digits[digit]));
            last_digits[digit] = digits[digit];
        }
    }
}

static bool parse_packet(char *packet, uint8_t values[8])
{
    char *cursor = packet;
    for (uint8_t i = 0; i < 8; ++i) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor != '-' && *cursor != '+' && !isdigit((unsigned char)*cursor)) return false;
        char *end;
        long value = strtol(cursor, &end, 10);
        if (end == cursor) return false;
        values[i] = value < 0 ? 0 : value > 8 ? 8 : (uint8_t)value;
        cursor = end;
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (i < 7) {
            if (*cursor++ != ',') return false;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi disconnected; reconnecting");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi ready, address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, WIFI_SSID, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, WIFI_PASSWORD, sizeof(station.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static void update_timeout(int64_t now_ms)
{
    if (now_ms - last_packet_ms <= PACKET_TIMEOUT_MS
        || now_ms - last_decay_ms < TIMEOUT_DECAY_STEP_MS) return;
    last_decay_ms = now_ms;
    bool changed = false;
    for (uint8_t i = 0; i < 8; ++i) {
        if (heights[i]) { --heights[i]; changed = true; }
    }
    if (changed) draw_bars();
}

static void udp_receiver_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno %d", errno);
        vTaskDelete(NULL);
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Listening for CSV heights on UDP port %d", UDP_PORT);
    for (;;) {
        char packet[64];
        int length = recvfrom(sock, packet, sizeof(packet) - 1, 0, NULL, NULL);
        if (length > 0 && !memchr(packet, '\0', length)) {
            packet[length] = '\0';
            uint8_t values[8];
            if (parse_packet(packet, values)) {
                memcpy(heights, values, sizeof(heights));
                last_packet_ms = esp_timer_get_time() / 1000;
                last_decay_ms = last_packet_ms;
                draw_bars();
            }
        } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "UDP receive error: errno %d", errno);
        }
        update_timeout(esp_timer_get_time() / 1000);
    }
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    matrix_init();
    wifi_init();
    ESP_ERROR_CHECK(xTaskCreate(udp_receiver_task, "udp_spectrum", 4096,
                               NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
