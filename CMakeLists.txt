cmake_minimum_required(VERSION 3.13)
project(pico8_downloader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include local headers, portlibs (cURL, SDL2, etc.), and libnx headers
include_directories(
    include
    $ENV{PORTLIBS}/switch/include
    $ENV{DEVKITPRO}/libnx/include
)

# Specify library search paths
link_directories(
    $ENV{PORTLIBS}/switch/lib
    $ENV{DEVKITPRO}/libnx/lib
)

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
