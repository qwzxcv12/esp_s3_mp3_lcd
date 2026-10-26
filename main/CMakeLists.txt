idf_component_register(SRCS "main.c" "st7735.c" "audio_test.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer freertos esp_driver_i2s esp_driver_spi)
