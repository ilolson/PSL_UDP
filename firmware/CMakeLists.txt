cmake_minimum_required(VERSION 3.13)

# Require the Pico SDK path
if(NOT DEFINED ENV{PICO_SDK_PATH})
  message(FATAL_ERROR "PICO_SDK_PATH not set. Example: export PICO_SDK_PATH=$HOME/pico-sdk")
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# If no board is set, default to the Pico W to enable its Wi-Fi stack
if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD pico_w CACHE STRING "" FORCE)
endif()
project(psl_udp C CXX ASM)
pico_sdk_init()

set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
# Pick up any .c/.cpp you drop into src/
file(GLOB APP_SOURCES CONFIGURE_DEPENDS
     "${SRC_DIR}/*.c" "${SRC_DIR}/*.cpp")

if(NOT APP_SOURCES)
  message(FATAL_ERROR "No sources found in ${SRC_DIR}. Add main.c/.cpp etc.")
endif()

add_executable(psl_udp ${APP_SOURCES})
pico_generate_pio_header(psl_udp ${SRC_DIR}/ws2812.pio)
target_include_directories(psl_udp PRIVATE ${SRC_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(psl_udp PRIVATE c_std_11)
target_compile_options(psl_udp PRIVATE -Wall -Wextra)

# TinyUSB is provided by the Pico SDK
target_link_libraries(psl_udp
    pico_stdlib
    pico_btstack_ble
    pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background
    pico_rand
    hardware_pio
    hardware_gpio
    hardware_clocks
)

target_compile_definitions(psl_udp PRIVATE CYW43_LWIP=0)

# Use USB stdio; disable UART stdio
pico_enable_stdio_usb(psl_udp 1)
pico_enable_stdio_uart(psl_udp 0)

# Produce UF2/ELF/BIN/MAP
pico_add_extra_outputs(psl_udp)
