idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES usb_host_hid esp_driver_gpio)
