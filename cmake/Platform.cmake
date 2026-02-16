# ============================================================================
# Platform Detection Module
# ============================================================================

set(NEUSTACK_PLATFORM_MACOS OFF)
set(NEUSTACK_PLATFORM_LINUX OFF)
set(NEUSTACK_PLATFORM_WINDOWS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(NEUSTACK_PLATFORM_MACOS ON)
    set(NEUSTACK_PLATFORM_NAME "macOS")
    message(STATUS "Platform: macOS - using utun device")

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NEUSTACK_PLATFORM_LINUX ON)
    set(NEUSTACK_PLATFORM_NAME "Linux")
    message(STATUS "Platform: Linux - using TAP/TUN device")

elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(NEUSTACK_PLATFORM_WINDOWS ON)
    set(NEUSTACK_PLATFORM_NAME "Windows")
    message(STATUS "Platform: Windows - using Wintun")

    # MinGW: 启用 C99 兼容 printf，支持 %zu/%zd
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)

else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# Export platform defines
if(NEUSTACK_PLATFORM_MACOS)
    add_compile_definitions(NEUSTACK_PLATFORM_MACOS)
elseif(NEUSTACK_PLATFORM_LINUX)
    add_compile_definitions(NEUSTACK_PLATFORM_LINUX)
elseif(NEUSTACK_PLATFORM_WINDOWS)
    add_compile_definitions(NEUSTACK_PLATFORM_WINDOWS)
endif()
