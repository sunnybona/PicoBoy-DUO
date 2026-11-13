cmake_minimum_required(VERSION 3.13..3.30)

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(picosystem_duo_bootloader C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

set(GB_LCD_SPI_HZ 80000000 CACHE STRING "像素 PIO 位时钟（Hz）")
set(GB_DISPLAY_SCALE15 1 CACHE STRING "1.5 倍放大（1=开，0=160x144 原尺寸）")

set(PICO_STACK_SIZE 8192)

set(GB_PIO_SRC "${CMAKE_CURRENT_LIST_DIR}/src/st7789_lcd.pio")

add_executable(duo_bootloader
    src/main.cpp
    src/display.cpp
    src/input.cpp
)

pico_generate_pio_header(duo_bootloader ${GB_PIO_SRC})

target_include_directories(duo_bootloader PRIVATE src)

target_compile_definitions(duo_bootloader PRIVATE
    GB_LCD_SPI_HZ=${GB_LCD_SPI_HZ}
    GB_DISPLAY_SCALE15=${GB_DISPLAY_SCALE15}
)

target_compile_options(duo_bootloader PRIVATE -O3 -g -Wall -Wextra
    -Wno-unused-parameter -Wno-sign-compare
    -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-missing-field-initializers -Wno-strict-aliasing
    -fno-strict-aliasing)

pico_enable_stdio_usb(duo_bootloader 1)
pico_enable_stdio_uart(duo_bootloader 0)

target_link_libraries(duo_bootloader PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_clocks
)

pico_add_extra_outputs(duo_bootloader)
