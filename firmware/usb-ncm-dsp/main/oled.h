#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OLED_SCL 21
#define OLED_SDA 14
#define OLED_ADDR 0x3C

bool oled_init(void);
bool oled_show(const char *title, const char *line);
bool oled_show_frame(const uint8_t frame[1024]);
