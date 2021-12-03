cmake_minimum_required(VERSION 3.13)

include(thirdparty/pico-sdk/pico_sdk_init.cmake)

project(ledmatrix_pico)

pico_sdk_init()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

set(ZSTD_MULTITHREAD_SUPPORT OFF)
add_subdirectory(thirdparty/zstd/build/cmake)
include_directories(thirdparty/zstd/lib)

add_definitions(-DWIFI_NAME="${WIFI_NAME}" -DWIFI_PASSWORD="${WIFI_PASSWORD}")


add_executable(ledmatrix_pico src/main.c src/esp-uart.h src/esp-uart.c)

pico_generate_pio_header(ledmatrix_pico ${CMAKE_CURRENT_SOURCE_DIR}/src/hub75.pio)


target_link_libraries(ledmatrix_pico libzstd_static pico_stdlib hardware_pio pico_multicore)
pico_add_extra_outputs(ledmatrix_pico)

pico_enable_stdio_usb(ledmatrix_pico 1)
pico_enable_stdio_uart(ledmatrix_pico 0)