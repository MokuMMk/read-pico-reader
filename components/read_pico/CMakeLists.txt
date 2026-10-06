# 板级层有两个实现，对外接口完全相同，按 PICO_BOARD 只编其中一个。
# Two board implementations share one public interface; PICO_BOARD picks which is compiled.
if(PICO_BOARD STREQUAL "metalio_eink4_plus")
    set(read_pico_board_src "read_pico_board_metalio.c")
else()
    set(read_pico_board_src "read_pico_board.c")
endif()
message(STATUS "read_pico board layer: ${read_pico_board_src}")

# Metalio 用厂商的波形表，本仓库那份波形组件不进依赖图（MINIMAL_BUILD 下不编）。
# The Metalio board uses the vendor's waveform tables, so this repo's waveform component is
# left out of the dependency graph and is not built under MINIMAL_BUILD.
if(PICO_BOARD STREQUAL "metalio_eink4_plus")
    set(read_pico_waveform_req "")
else()
    set(read_pico_waveform_req e0470_epaper_waveform)
endif()

set(read_pico_requires epdiy ${read_pico_waveform_req} sc7a20h cst836u read_pico_pmu fatfs sy7636a fca9555 tps65185
                   esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_rmt esp_driver_sdmmc pwm_audio)

idf_component_register(
    SRCS "read_pico_init.c" ${read_pico_board_src} "read_pico_epd_timing.c" "read_pico_buzzer.c" "buzzer_score.c" "buzzer_duty_map.c" "buzzer_1bit.c" "read_pico_sd.c" "read_pico_flash_hpm.c"
    INCLUDE_DIRS "include"
    REQUIRES ${read_pico_requires}
    PRIV_REQUIRES bootloader_support spi_flash freertos
    # flash_hpm_zb.c 只定义了一个覆盖 IDF weak 符号的表，没有任何
    # 代码引用它，不整包链接的话这个目标文件会被丢掉，覆盖就不生效。
    WHOLE_ARCHIVE
)
