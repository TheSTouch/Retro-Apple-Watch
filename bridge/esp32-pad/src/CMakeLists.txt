idf_component_register(SRCS "main.c" "pad_state.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_driver_gpio)
