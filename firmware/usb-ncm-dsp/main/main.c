#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "driver/spi_master.h"
#include "audio_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "oled.h"
#include "usb_network.h"

#define UDP_PORT 4210

#define LED_MATRIX_CLK 18
#define LED_MATRIX_DIN 16
#define LED_MATRIX_LOAD 17

#define MATRIX_INTENSITY 4          // 0..15
#define PACKET_TIMEOUT_MS 200
#define TIMEOUT_DECAY_STEP_MS 50

#define CENTER_MIRROR 1            // Bass center, then mids/presence/treble toward edges

// Logical (0,0) is the top-left pixel. Adjust these for your module.
#define MATRIX_ROTATION 0          // 0, 90, 180, or 270 degrees clockwise
#define MIRROR_X 0                 // Reverse left/right after choosing the source band
#define MIRROR_Y 0                 // Flip top/bottom
#define DIGITS_ARE_COLUMNS 1       // 1: digit register is a column; use 0 for row-wired modules
#define DEBUG_PACKETS 0            // Set to 1 to log each valid UDP packet

static const char *TAG = "spectrum";
static spi_device_handle_t matrix_spi;
static uint8_t heights[8];
static uint8_t last_digits[8];
static int64_t last_packet_ms;
static int64_t last_decay_ms;
static QueueHandle_t display_queue;
static uint32_t last_audio_sequence;
static bool have_audio_sequence;

typedef enum {
    DISPLAY_VIDEO,
    DISPLAY_LYRICS,
} display_message_type_t;

typedef struct {
    display_message_type_t type;
    union {
        struct {
            uint32_t sequence;
            uint8_t pixels[1024];
        } video;
        struct {
            char title[64];
            char line[64];
        } lyrics;
    } payload;
} display_message_t;

static esp_err_t max7219_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    spi_transaction_t transaction = {
        .length = 16,  // SPI transaction length is in bits.
        .tx_buffer = tx,
    };
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

    ESP_ERROR_CHECK(max7219_write(0x0C, 0));                  // Shutdown while configuring
    ESP_ERROR_CHECK(max7219_write(0x0F, 0));                  // Display test off
    ESP_ERROR_CHECK(max7219_write(0x09, 0));                  // No BCD decoding
    ESP_ERROR_CHECK(max7219_write(0x0B, 7));                  // Scan all eight digits
    ESP_ERROR_CHECK(max7219_write(0x0A, MATRIX_INTENSITY));
    for (uint8_t digit = 0; digit < 8; ++digit) {
        ESP_ERROR_CHECK(max7219_write(digit + 1, 0));
    }
    ESP_ERROR_CHECK(max7219_write(0x0C, 1));                  // Normal operation
}

static void map_pixel(uint8_t x, uint8_t y, uint8_t *digit, uint8_t *bit)
{
    if (MIRROR_X) x = 7 - x;
    if (MIRROR_Y) y = 7 - y;

    uint8_t physical_x = x;
    uint8_t physical_y = y;
    switch (MATRIX_ROTATION) {
    case 90:  physical_x = 7 - y; physical_y = x;     break;
    case 180: physical_x = 7 - x; physical_y = 7 - y; break;
    case 270: physical_x = y;     physical_y = 7 - x; break;
    default: break;
    }

    if (DIGITS_ARE_COLUMNS) {
        *digit = physical_x;
        *bit = 7 - physical_y;
    } else {
        *digit = physical_y;
        *bit = 7 - physical_x;
    }
}

static uint8_t taller(uint8_t a, uint8_t b)
{
    return a > b ? a : b;
}

static void layout_bars(uint8_t display[8])
{
#if CENTER_MIRROR
    uint8_t bass = taller(heights[0], heights[1]);
    uint8_t mids = taller(heights[2], heights[3]);
    uint8_t presence = taller(heights[4], heights[5]);
    uint8_t treble = taller(heights[6], heights[7]);
    display[0] = display[7] = treble;
    display[1] = display[6] = presence;
    display[2] = display[5] = mids;
    display[3] = display[4] = bass;
#else
    memcpy(display, heights, 8);
#endif
}

static void draw_bars(void)
{
    uint8_t digits[8] = {0};
    uint8_t display[8];
    layout_bars(display);
    for (uint8_t x = 0; x < 8; ++x) {
        for (uint8_t y = 8 - display[x]; y < 8; ++y) {
            uint8_t digit, bit;
            map_pixel(x, y, &digit, &bit);
            digits[digit] |= (uint8_t)(1U << bit);
        }
    }
    for (uint8_t digit = 0; digit < 8; ++digit) {
        if (digits[digit] != last_digits[digit]) {
            esp_err_t err = max7219_write(digit + 1, digits[digit]);
            if (err == ESP_OK) {
                last_digits[digit] = digits[digit];
            } else {
                ESP_LOGE(TAG, "MAX7219 SPI write failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static bool parse_packet(uint8_t *packet, uint8_t values[8])
{
    char *cursor = (char *)packet;
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

static bool parse_video_packet(const uint8_t *packet, size_t length,
                               display_message_t *message)
{
    if (length != 5 + sizeof(message->payload.video.pixels) || packet[0] != 'V') return false;
    uint32_t sequence;
    memcpy(&sequence, packet + 1, sizeof(sequence));
    message->type = DISPLAY_VIDEO;
    message->payload.video.sequence = ntohl(sequence);
    memcpy(message->payload.video.pixels, packet + 5,
           sizeof(message->payload.video.pixels));
    return true;
}

static bool parse_lyrics_packet(const uint8_t *packet, size_t length,
                                display_message_t *message)
{
    if (length < 3 || packet[0] != 'L') return false;
    const size_t title_length = packet[1];
    const size_t line_length = packet[2];
    if (title_length >= sizeof(message->payload.lyrics.title)
        || line_length >= sizeof(message->payload.lyrics.line)
        || length != 3 + title_length + line_length
        || memchr(packet + 3, '\0', title_length + line_length) != NULL) {
        return false;
    }

    message->type = DISPLAY_LYRICS;
    memcpy(message->payload.lyrics.title, packet + 3, title_length);
    message->payload.lyrics.title[title_length] = '\0';
    memcpy(message->payload.lyrics.line, packet + 3 + title_length, line_length);
    message->payload.lyrics.line[line_length] = '\0';
    return true;
}

static bool process_audio_packet(const uint8_t *packet, size_t length)
{
    if (length != 5 + AUDIO_DSP_PACKET_SIZE || packet[0] != 'A') return false;

    uint32_t sequence_network;
    memcpy(&sequence_network, packet + 1, sizeof(sequence_network));
    const uint32_t sequence = ntohl(sequence_network);
    if (have_audio_sequence && sequence != last_audio_sequence + 1) {
#if DEBUG_PACKETS
        ESP_LOGW(TAG, "audio sequence gap: expected %lu, got %lu",
                 (unsigned long)(last_audio_sequence + 1), (unsigned long)sequence);
#endif
    }
    last_audio_sequence = sequence;
    have_audio_sequence = true;

    int16_t samples[AUDIO_DSP_HOP_SIZE];
    const uint8_t *pcm = packet + 5;
    for (unsigned i = 0; i < AUDIO_DSP_HOP_SIZE; ++i) {
        const uint16_t raw = (uint16_t)pcm[2 * i]
                           | ((uint16_t)pcm[2 * i + 1] << 8);
        samples[i] = (int16_t)raw;
    }

    if (!audio_dsp_process_hop(samples, heights)) return false;
    last_packet_ms = esp_timer_get_time() / 1000;
    last_decay_ms = last_packet_ms;
    draw_bars();
    return true;
}

static void oled_task(void *arg)
{
    (void)arg;
    display_message_t incoming;
    bool ready = false;
    bool have_frame = false;
    for (;;) {
        if (!ready) {
            ready = oled_init();
            if (ready) oled_show("YOUTUBE", "Waiting for video");
        }
        TickType_t wait = ready ? pdMS_TO_TICKS(5000) : pdMS_TO_TICKS(2000);
        if (xQueueReceive(display_queue, &incoming, wait) == pdTRUE) {
            have_frame = true;
            if (ready) {
                bool shown = incoming.type == DISPLAY_VIDEO
                    ? oled_show_frame(incoming.payload.video.pixels)
                    : oled_show(incoming.payload.lyrics.title,
                                incoming.payload.lyrics.line);
                if (!shown) ready = false;
            }
        } else if (ready && have_frame) {
            have_frame = false;
            if (!oled_show("VIDEO", "Signal lost")) ready = false;
        } else if (ready) {
            oled_show("YOUTUBE", "Waiting for video");
        }
    }
}

static void update_timeout(int64_t now_ms)
{
    if (now_ms - last_packet_ms <= PACKET_TIMEOUT_MS) return;
    if (now_ms - last_decay_ms < TIMEOUT_DECAY_STEP_MS) return;
    last_decay_ms = now_ms;

    bool changed = false;
    for (uint8_t i = 0; i < 8; ++i) {
        if (heights[i] > 0) {
            --heights[i];
            changed = true;
        }
    }
    if (changed) draw_bars();
}

static void udp_receiver_task(void *arg)
{
    (void)arg;
    for (;;) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
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
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "Listening for PCM audio, CSV bars, video, and lyrics on UDP port %d", UDP_PORT);

        for (;;) {
            uint8_t packet[1200];
            int length = recvfrom(sock, packet, sizeof(packet) - 1, 0, NULL, NULL);
            if (length > 0) {
                if (packet[0] == 'A') {
                    if (!process_audio_packet(packet, length)) {
#if DEBUG_PACKETS
                        ESP_LOGW(TAG, "rejected audio packet (%d bytes)", length);
#endif
                    }
                } else if (packet[0] == 'V') {
                    display_message_t message;
                    if (parse_video_packet(packet, length, &message)) {
                        xQueueOverwrite(display_queue, &message);
                    }
                } else if (packet[0] == 'L') {
                    display_message_t message;
                    if (parse_lyrics_packet(packet, length, &message)) {
                        xQueueOverwrite(display_queue, &message);
                    }
                } else if (memchr(packet, '\0', length) == NULL) {
                    packet[length] = '\0';
                    uint8_t values[8];
                    if (parse_packet(packet, values)) {
#if DEBUG_PACKETS
                        ESP_LOGI(TAG, "packet: %s", packet);
#endif
                        memcpy(heights, values, sizeof(heights));
                        last_packet_ms = esp_timer_get_time() / 1000;
                        last_decay_ms = last_packet_ms;
                        draw_bars();
                    }
                }
            } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "UDP receive failed: errno %d", errno);
                break;
            }
            update_timeout(esp_timer_get_time() / 1000);
        }
        close(sock);
    }
}

void app_main(void)
{
    // Start USB before peripherals so the host link is ready as soon as the app boots.
    ESP_ERROR_CHECK(usb_network_init());
    matrix_init();
    ESP_ERROR_CHECK(audio_dsp_init());
    display_queue = xQueueCreate(1, sizeof(display_message_t));
    ESP_ERROR_CHECK(display_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(oled_task, "oled_video", 4096, NULL, 4, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(udp_receiver_task, "udp_spectrum", 8192, NULL, 5, NULL);
}
