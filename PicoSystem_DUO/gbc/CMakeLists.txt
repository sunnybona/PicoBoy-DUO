cmake_minimum_required(VERSION 3.13..3.30)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(rp2040_gbc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# ---- 编译期配置（都可以用 -D 覆盖） ------------------------------------
set(GB_AUDIO_SAMPLE_RATE 61035 CACHE STRING "AUDIO_SAMPLE_RATE（= PWM 载波频率）")
set(GB_LCD_SPI_HZ 80000000 CACHE STRING "像素 PIO 位时钟（Hz）")
set(GB_DISPLAY_SCALE15 1 CACHE STRING "1.5 倍放大（1=开，0=160x144 原尺寸）")

set(PICO_STACK_SIZE 8192)

# 16MB flash；设置块与 ROM 区均为绝对地址，不依赖内嵌 ROM 机制。
set(GB_FLASH_SIZE_BYTES 16777216 CACHE STRING "板载 flash 容量（字节）")

# ---- 目标 ------------------------------------------------------------
set(GB_PIO_SRC "${CMAKE_CURRENT_LIST_DIR}/src/st7789_lcd.pio")

add_executable(rp2040_gbc
    src/main.cpp
    src/display.cpp
    src/audio.cpp
    src/input.cpp
    src/tar.cpp
    lib/minigb_apu/minigb_apu.c
)

pico_generate_pio_header(rp2040_gbc ${GB_PIO_SRC})

# 槽位固件：链接在 0x10040000，由 bootloader 跳入。
pico_set_linker_script(rp2040_gbc ${CMAKE_CURRENT_LIST_DIR}/memmap_slot.ld)

target_include_directories(rp2040_gbc PRIVATE
    src
    lib/walnut
    lib/minigb_apu
)

target_compile_definitions(rp2040_gbc PRIVATE
    AUDIO_SAMPLE_RATE=${GB_AUDIO_SAMPLE_RATE}
    MINIGB_APU_AUDIO_FORMAT_S16SYS
    GB_LCD_SPI_HZ=${GB_LCD_SPI_HZ}
    GB_DISPLAY_SCALE15=${GB_DISPLAY_SCALE15}
    PICO_FLASH_SIZE_BYTES=${GB_FLASH_SIZE_BYTES}
)

target_compile_options(rp2040_gbc PRIVATE -O3 -g -Wall -Wextra
    -Wno-unused-parameter -Wno-sign-compare
    -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-missing-field-initializers -Wno-strict-aliasing
    -fno-strict-aliasing)

pico_enable_stdio_usb(rp2040_gbc 1)
pico_enable_stdio_uart(rp2040_gbc 0)

target_link_libraries(rp2040_gbc PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_flash
    hardware_clocks
)

pico_add_extra_outputs(rp2040_gbc)
