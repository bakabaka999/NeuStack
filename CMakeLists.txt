cmake_minimum_required(VERSION 3.20)

project(NeuStack
    VERSION 0.1.0
    DESCRIPTION "Cross-platform User-space TCP/IP Stack with AI-powered Congestion Control"
    LANGUAGES CXX
)

# ============================================================================
# Build Options
# ============================================================================
option(NEUSTACK_BUILD_TESTS "Build unit tests" ON)
option(NEUSTACK_BUILD_EXAMPLES "Build examples" ON)
option(NEUSTACK_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(NEUSTACK_ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(NEUSTACK_ENABLE_AI "Enable AI congestion control (requires ONNX Runtime)" OFF)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Platform Detection
# ============================================================================
include(cmake/Platform.cmake)

# ============================================================================
# Compiler Flags
# ============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror=return-type
        -Wno-unused-parameter
    )

    if(NEUSTACK_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/W4 /utf-8)
endif()

# ============================================================================
# Include Directories
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Source Files
# ============================================================================

# Common sources (cross-platform)
set(COMMON_SOURCES
    src/common/checksum.cpp
    src/common/ip_addr.cpp
)

# Network layer sources
set(NET_SOURCES
    src/net/ipv4.cpp
    src/net/icmp.cpp
    # src/net/arp.cpp
)

# Transport layer sources
set(TRANSPORT_SOURCES
    src/transport/udp.cpp
    # src/transport/tcp_conn.cpp
    # src/transport/tcp_state.cpp
    # src/transport/tcp_window.cpp
    # src/transport/tcp_timer.cpp
)

# Platform-specific HAL sources
if(NEUSTACK_PLATFORM_MACOS)
    set(HAL_SOURCES
        src/hal/device.cpp
        src/hal/hal_macos.cpp
    )
elseif(NEUSTACK_PLATFORM_LINUX)
    set(HAL_SOURCES
        src/hal/device.cpp
        src/hal/hal_linux.cpp
    )
elseif(NEUSTACK_PLATFORM_WINDOWS)
    set(HAL_SOURCES
        src/hal/device.cpp
        src/hal/hal_windows.cpp
    )
endif()

# AI sources (optional)
if(NEUSTACK_ENABLE_AI)
    set(AI_SOURCES
        # src/ai/cc_model.cpp
    )
    find_package(onnxruntime QUIET)
    if(NOT onnxruntime_FOUND)
        message(WARNING "ONNX Runtime not found, AI features disabled")
        set(NEUSTACK_ENABLE_AI OFF)
    endif()
endif()

# ============================================================================
# Main Library
# ============================================================================
add_library(neustack_lib STATIC
    ${COMMON_SOURCES}
    ${NET_SOURCES}
    ${TRANSPORT_SOURCES}
    ${HAL_SOURCES}
    ${AI_SOURCES}
)

target_include_directories(neustack_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Platform-specific libraries
if(NEUSTACK_PLATFORM_MACOS)
    target_link_libraries(neustack_lib PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
elseif(NEUSTACK_PLATFORM_LINUX)
    # Linux doesn't need extra libraries for TUN/TAP
elseif(NEUSTACK_PLATFORM_WINDOWS)
    target_link_libraries(neustack_lib PRIVATE ws2_32 iphlpapi)
endif()

if(NEUSTACK_ENABLE_AI)
    target_link_libraries(neustack_lib PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(neustack_lib PUBLIC NEUSTACK_AI_ENABLED)
endif()

# ============================================================================
# Main Executable
# ============================================================================
add_executable(neustack src/main.cpp)
target_link_libraries(neustack PRIVATE neustack_lib)

# ============================================================================
# Tests
# ============================================================================
if(NEUSTACK_BUILD_TESTS)
    enable_testing()

    # Find Catch2 (optional, will skip tests if not found)
    find_package(Catch2 3 QUIET)

    if(Catch2_FOUND)
        add_subdirectory(tests)
    else()
        message(STATUS "Catch2 not found, tests will not be built")
    endif()
endif()

# ============================================================================
# Examples
# ============================================================================
if(NEUSTACK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Install
# ============================================================================
install(TARGETS neustack neustack_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/neustack DESTINATION include)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "========== NeuStack Build Configuration ==========")
message(STATUS "Version:          ${PROJECT_VERSION}")
message(STATUS "Platform:         ${NEUSTACK_PLATFORM_NAME}")
message(STATUS "C++ Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Tests:      ${NEUSTACK_BUILD_TESTS}")
message(STATUS "Build Examples:   ${NEUSTACK_BUILD_EXAMPLES}")
message(STATUS "Enable ASan:      ${NEUSTACK_ENABLE_ASAN}")
message(STATUS "Enable AI:        ${NEUSTACK_ENABLE_AI}")
message(STATUS "==================================================")
message(STATUS "")
