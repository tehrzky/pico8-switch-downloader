cmake_minimum_required(VERSION 3.13)
project(pico8_downloader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

file(GLOB SOURCES "source/*.cpp")

add_executable(pico8_downloader.elf ${SOURCES})

target_link_libraries(pico8_downloader.elf
    curl
    mbedtls
    mbedcrypto
    mbedx509
    z
    sdl2
    sdl2_image
    sdl2_ttf
    nx
)
