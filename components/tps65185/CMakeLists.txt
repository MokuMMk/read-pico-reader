# TPS65185 电子纸电源管理芯片（Metalio E-Ink4-Plus 的 EPD 电源轨）。
# TPS65185 e-paper PMIC, the EPD rail controller on the Metalio E-Ink4-Plus.
idf_component_register(
    SRCS "tps65185.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_timer
)
