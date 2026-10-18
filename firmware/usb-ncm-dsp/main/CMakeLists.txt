idf_component_register(SRCS "main.c" "audio_dsp.c" "usb_network.c" "oled.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_netif esp_driver_spi esp_driver_i2c esp_timer lwip)
