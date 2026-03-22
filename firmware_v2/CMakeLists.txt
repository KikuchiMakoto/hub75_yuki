cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(firmware_v2 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CPU_CLOCK_KHZ 200000 CACHE STRING "RP2040 system clock kHz")
set(MAX_FRAME_UPDATE_FPS 180 CACHE STRING "Display swap FPS cap")

pico_sdk_init()

add_executable(firmware_v2
    src/main.c
    src/cobs.c
    src/syscalls_stub.c
    src/usb_descriptors.c
)

pico_generate_pio_header(firmware_v2 ${CMAKE_CURRENT_LIST_DIR}/src/hub75.pio)

target_include_directories(firmware_v2 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_definitions(firmware_v2 PRIVATE
    CPU_CLOCK_KHZ=${CPU_CLOCK_KHZ}
    MAX_FRAME_UPDATE_FPS=${MAX_FRAME_UPDATE_FPS}
    NDEBUG
)

target_link_libraries(firmware_v2
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_clocks
    tinyusb_device
    tinyusb_board
)

if(TARGET hardware_vreg)
    target_link_libraries(firmware_v2 hardware_vreg)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(firmware_v2 PRIVATE
        -Ofast
        -flto
        -fomit-frame-pointer
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables
    )

    target_link_options(firmware_v2 PRIVATE
        -flto
        -Wl,--gc-sections
    )
endif()

pico_enable_stdio_usb(firmware_v2 0)
pico_enable_stdio_uart(firmware_v2 0)

pico_add_extra_outputs(firmware_v2)
