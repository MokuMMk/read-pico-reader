idf_component_register(
    SRCS "fca9555.c" "fca9555_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
)
