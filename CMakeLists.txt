cmake_minimum_required(VERSION 3.16)
# Odev: sender, receiver ve emulator yalnizca C + POSIX + standart C kutuphanesi
project(network_hw VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

option(BUILD_SENDER     "Build sender target"   ON)
option(BUILD_RECEIVER   "Build receiver target" ON)
option(BUILD_EMULATOR   "Build emulator target" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer in Debug" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Tum calistirilabilir dosyalar build/<preset>/bin altina
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ortak header'lar (paket formati, CRC-32 vb.) icin arayuz kutuphanesi
add_library(rdt_common INTERFACE)
target_include_directories(rdt_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/config)
# POSIX API'leri (clock_gettime, getopt, sockets...) -std=c11 ile gorunur olsun
target_compile_definitions(rdt_common INTERFACE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

# Debug ve uyari bayraklarini hedefe uygulayan yardimci fonksiyon
function(apply_target_debug_config target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    target_link_libraries(${target_name} PRIVATE rdt_common)

    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        $<$<CONFIG:Debug>:-g3 -O0 -fno-omit-frame-pointer>
        $<$<CONFIG:RelWithDebInfo>:-g -O2>
        $<$<CONFIG:Release>:-O2>
    )

    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:DEBUG_BUILD=1>
        $<$<CONFIG:Release>:NDEBUG>
    )

    if(ENABLE_SANITIZERS)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
        target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
    endif()
endfunction()

if(BUILD_SENDER)
    add_subdirectory(sender)
    apply_target_debug_config(sender)
endif()

if(BUILD_RECEIVER)
    add_subdirectory(receiver)
    apply_target_debug_config(receiver)
endif()

if(BUILD_EMULATOR)
    add_subdirectory(emulator)
    apply_target_debug_config(emulator)
endif()
