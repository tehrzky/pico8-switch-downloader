cmake_minimum_required(VERSION 3.13)
project(pico8_downloader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEVKITPRO /opt/devkitpro)
if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO $ENV{DEVKITPRO})
endif()

file(GLOB SOURCES "source/*.cpp")

add_executable(pico8_downloader ${SOURCES})

# Force output filename to be pico8_downloader.elf
set_target_properties(pico8_downloader PROPERTIES SUFFIX ".elf")

target_include_directories(pico8_downloader PRIVATE
    include
    ${DEVKITPRO}/portlibs/switch/include
    ${DEVKITPRO}/libnx/include
)

target_link_directories(pico8_downloader PRIVATE
    ${DEVKITPRO}/portlibs/switch/lib
    ${DEVKITPRO}/libnx/lib
)

# devkitPro portlibs require exact case-sensitive names (SDL2, SDL2_image, SDL2_ttf)
target_link_libraries(pico8_downloader PRIVATE
    curl
    mbedtls
    mbedcrypto
    mbedx509
    z
    SDL2
    SDL2_image
    SDL2_ttf
    nx
)
