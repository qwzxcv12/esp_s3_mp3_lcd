idf_component_register(SRCS "main.c" "audio_synth.c" "led_effects.c"
                    INCLUDE_DIRS "."
                    REQUIRES usb_host_hid esp_driver_gpio esp_driver_i2s esp_driver_rmt led_strip esp_psram)
