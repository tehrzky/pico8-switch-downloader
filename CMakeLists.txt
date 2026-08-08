cmake_minimum_required(VERSION 3.13)
project(pico8_downloader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolve DEVKITPRO root path
set(DEVKITPRO /opt/devkitpro)
if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO $ENV{DEVKITPRO})
endif()

# Include directories for local headers, portlibs (curl, sdl2), and libnx
include_directories(
    include
    ${DEVKITPRO}/portlibs/switch/include
    ${DEVKITPRO}/libnx/include
)

# Search directories for libraries
link_directories(
    ${DEVKITPRO}/portlibs/switch/lib
    ${DEVKITPRO}/libnx/lib
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
