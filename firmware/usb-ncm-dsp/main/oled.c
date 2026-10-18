#include "oled.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_TIMEOUT_MS 100

static const char *TAG = "oled";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static uint8_t pixels[OLED_WIDTH * OLED_HEIGHT / 8];

// Five columns per glyph, bit 0 at the top. Text is displayed in uppercase.
static const uint8_t letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x41,0x41,0x7f,0x41,0x41}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t digits[10][5] = {
    {0x3e,0x45,0x49,0x51,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};

static void glyph(char c, uint8_t out[5])
{
    c = (char)toupper((unsigned char)c);
    const uint8_t *source = NULL;
    if (c >= 'A' && c <= 'Z') source = letters[c - 'A'];
    else if (c >= '0' && c <= '9') source = digits[c - '0'];
    if (source) {
        memcpy(out, source, 5);
        return;
    }
    memset(out, 0, 5);
    switch (c) {
    case '-': out[0] = out[1] = out[2] = out[3] = out[4] = 0x08; break;
    case '.': out[2] = 0x60; break;
    case ',': out[2] = 0x60; out[1] = 0x40; break;
    case '!': out[2] = 0x5f; break;
    case '?': out[0] = 0x02; out[1] = 0x01; out[2] = 0x51;
              out[3] = 0x09; out[4] = 0x06; break;
    case '\'': out[2] = 0x03; break;
    case ':': out[2] = 0x36; break;
    case '/': out[0] = 0x20; out[1] = 0x10; out[2] = 0x08;
              out[3] = 0x04; out[4] = 0x02; break;
    case '&': out[0] = 0x36; out[1] = 0x49; out[2] = 0x55;
              out[3] = 0x22; out[4] = 0x50; break;
    case '+': out[1] = out[2] = out[3] = 0x08; out[2] = 0x3e; break;
    case ' ': break;
    default: out[0] = 0x02; out[1] = 0x01; out[2] = 0x51;
             out[3] = 0x09; out[4] = 0x06; break;
    }
}

static void draw_char(int x, int y, char c)
{
    if (x < 0 || y < 0 || x + 5 > OLED_WIDTH || y + 7 > OLED_HEIGHT) return;
    uint8_t columns[5];
    glyph(c, columns);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (columns[col] & (1U << row)) {
                int py = y + row;
                pixels[(py / 8) * OLED_WIDTH + x + col] |= 1U << (py % 8);
            }
        }
    }
}

static void draw_text_centered(int y, const char *text, size_t length)
{
    if (length > 21) length = 21;
    int x = (OLED_WIDTH - (int)length * 6) / 2;
    for (size_t i = 0; i < length; ++i) draw_char(x + (int)i * 6, y, text[i]);
}

static esp_err_t command(const uint8_t *bytes, size_t length)
{
    uint8_t packet[32] = {0x00};
    if (length > sizeof(packet) - 1) return ESP_ERR_INVALID_ARG;
    memcpy(packet + 1, bytes, length);
    return i2c_master_transmit(device, packet, length + 1, OLED_TIMEOUT_MS);
}

static esp_err_t flush(void)
{
    for (uint8_t page = 0; page < 8; ++page) {
        const uint8_t address[] = {(uint8_t)(0xb0 | page), 0x00, 0x10};
        esp_err_t error = command(address, sizeof(address));
        if (error != ESP_OK) return error;
        for (int x = 0; x < OLED_WIDTH; x += 32) {
            uint8_t packet[33] = {0x40};
            memcpy(packet + 1, &pixels[page * OLED_WIDTH + x], 32);
            error = i2c_master_transmit(device, packet, sizeof(packet), OLED_TIMEOUT_MS);
            if (error != ESP_OK) return error;
        }
    }
    return ESP_OK;
}

bool oled_init(void)
{
    if (!bus) {
        i2c_master_bus_config_t config = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_NUM_0,
            .sda_io_num = OLED_SDA,
            .scl_io_num = OLED_SCL,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t error = i2c_new_master_bus(&config, &bus);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus setup failed: %s", esp_err_to_name(error));
            return false;
        }
    }
    esp_err_t error = i2c_master_probe(bus, OLED_ADDR, OLED_TIMEOUT_MS);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "No OLED ACK at 0x%02x on SDA=%d SCL=%d: %s",
                 OLED_ADDR, OLED_SDA, OLED_SCL, esp_err_to_name(error));
        return false;
    }
    if (!device) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_ADDR,
            .scl_speed_hz = 400000,
        };
        error = i2c_master_bus_add_device(bus, &config, &device);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "OLED device setup failed: %s", esp_err_to_name(error));
            return false;
        }
    }
    const uint8_t init[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x02, 0xa1, 0xc8, 0xda, 0x12,
        0x81, 0x7f, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0xaf,
    };
    error = command(init, sizeof(init));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 initialization failed: %s", esp_err_to_name(error));
        return false;
    }
    memset(pixels, 0, sizeof(pixels));
    error = flush();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 clear failed: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(TAG, "SSD1306 128x64 ready on SDA=%d SCL=%d at 0x%02x",
             OLED_SDA, OLED_SCL, OLED_ADDR);
    return true;
}

bool oled_show(const char *title, const char *line)
{
    if (!device) return false;
    memset(pixels, 0, sizeof(pixels));

    draw_text_centered(0, title, strlen(title));
    for (int x = 0; x < OLED_WIDTH; ++x) {
        pixels[(12 / 8) * OLED_WIDTH + x] |= 1U << (12 % 8);
    }

    // Wrap on spaces into at most five centered 21-character lines.
    const char *cursor = line;
    for (int row = 0; row < 5 && *cursor; ++row) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        size_t remaining = strlen(cursor);
        size_t length = remaining > 21 ? 21 : remaining;
        if (remaining > 21) {
            const char *space = NULL;
            for (size_t i = 0; i < length; ++i) {
                if (cursor[i] == ' ') space = cursor + i;
            }
            if (space && space > cursor) length = (size_t)(space - cursor);
        }
        draw_text_centered(18 + row * 9, cursor, length);
        cursor += length;
    }
    esp_err_t error = flush();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OLED transfer failed: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

bool oled_show_frame(const uint8_t frame[1024])
{
    if (!device || !frame) return false;
    memcpy(pixels, frame, sizeof(pixels));
    esp_err_t error = flush();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OLED frame transfer failed: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}
