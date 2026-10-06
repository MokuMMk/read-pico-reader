# ESP32-S31 的 Metalio 板用 FT6336U，Read Pico 用 CST836U。
# The ESP32-S31 Metalio board uses an FT6336U; Read Pico uses a CST836U.
if(PICO_BOARD STREQUAL "metalio_eink4_plus")
    set(cst836u_src "cst836u_ft6336u.c")
else()
    set(cst836u_src "cst836u.c")
endif()
message(STATUS "cst836u backend: ${cst836u_src}")

idf_component_register(
    # 接口同名，两颗芯片各一个实现，按板二选一。
    # Same interface, one implementation per controller, chosen by board.
    SRCS ${cst836u_src}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES freertos
)
