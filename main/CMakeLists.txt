idf_component_register(SRCS "main.c" "st7735.c" "audio_test.c" "mic_test.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer freertos nvs_flash esp_psram esp_driver_i2s esp_driver_spi)
