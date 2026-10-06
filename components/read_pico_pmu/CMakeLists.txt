# Metalio（ESP32-S31）没有 CW32：电量走 BQ27220、按键走 TCA9555、时间走 PCF8563。
# The Metalio board (ESP32-S31) has no CW32: the gauge is a BQ27220, the keys sit on the TCA9555
# and the clock is a PCF8563.
#
# 本组件不能 require read_pico——read_pico 已经 require 了本组件，那会成环。
# This component must not require read_pico, because read_pico already requires it.
#
# idf_component_register 必须留在顶层：放进 if/else 里时 SRCS 会生效而 REQUIRES 不会，
# fca9555 的头路径传不过来。依赖对两块板都无害（Pico 本来就用 9555），所以写死。
# idf_component_register must stay at the top level: inside an if/else its SRCS takes effect but
# its REQUIRES does not, so fca9555's include dirs never arrive. The dependency is harmless on both
# boards (Read Pico already uses a 9555), so it is unconditional.
if(PICO_BOARD STREQUAL "metalio_eink4_plus")
    set(pmu_src "read_pico_pmu_metalio.c")
else()
    set(pmu_src "read_pico_pmu.c")
endif()
message(STATUS "read_pico_pmu backend: ${pmu_src}")

idf_component_register(
    SRCS ${pmu_src}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c fca9555
    PRIV_REQUIRES freertos esp_rom
)
