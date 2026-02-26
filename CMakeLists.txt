cmake_minimum_required(VERSION 3.20)

project(NeuStack
    VERSION 1.2.0
    DESCRIPTION "Cross-platform User-space TCP/IP Stack with AI-powered Congestion Control and Intelligent Firewall"
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
set(CMAKE_CXX_STANDARD 20)
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

# MinGW %zu support: handled in cmake/Platform.cmake

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
    src/common/json_builder.cpp
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
    src/transport/tcp_segment.cpp
    src/transport/tcp_builder.cpp
    src/transport/tcp_connection.cpp
    src/transport/tcp_layer.cpp
)

# Application layer sources
set(APP_SOURCES
    src/app/http_types.cpp
    src/app/http_parser.cpp
    src/app/http_server.cpp
    src/app/http_client.cpp
    src/app/dns_client.cpp
    src/neustack.cpp
)

# Metrics sources (AI feature extraction, always compiled)
set(METRICS_SOURCES
    src/metrics/ai_features.cpp
)

# Telemetry sources (v1.3: observability)
set(TELEMETRY_SOURCES
    src/telemetry/metrics_registry.cpp
    src/telemetry/json_exporter.cpp
    src/telemetry/prometheus_exporter.cpp
    src/telemetry/telemetry_api.cpp
    src/telemetry/http_endpoints.cpp
)

set(FIREWALL_SOURCES
    src/firewall/firewall_engine.cpp
    src/firewall/firewall_ai.cpp
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
    # Wintun: loaded at runtime, no build-time dependency
    # User must place wintun.dll in the working directory
    # Download: https://www.wintun.net/
endif()

# AI sources (optional)
if(NEUSTACK_ENABLE_AI)
    # 使用自定义查找模块
    include(cmake/FindONNXRuntime.cmake)

    if(ONNXRUNTIME_FOUND)
        set(AI_SOURCES
            src/ai/onnx_inference.cpp
            src/ai/orca_model.cpp
            src/ai/anomaly_model.cpp
            src/ai/bandwidth_model.cpp
            src/ai/security_model.cpp
            src/ai/intelligence_plane.cpp
            src/ai/ai_agent.cpp
        )
    else()
        message(WARNING "ONNX Runtime not found, AI features disabled")
        message(STATUS "  Run: ./scripts/download_onnxruntime.sh")
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
    ${APP_SOURCES}
    ${METRICS_SOURCES}
    ${TELEMETRY_SOURCES}
    ${FIREWALL_SOURCES}
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

if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    target_include_directories(neustack_lib PUBLIC
        $<BUILD_INTERFACE:${ONNXRUNTIME_INCLUDE_DIRS}>
    )
    target_link_libraries(neustack_lib PRIVATE ${ONNXRUNTIME_LIBRARIES})
    target_compile_definitions(neustack_lib PUBLIC NEUSTACK_AI_ENABLED)
endif()

# MinGW: 确保 %zu 等格式符支持 (target 级别，比目录级更可靠)
if(MINGW OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    target_compile_definitions(neustack_lib PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

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
include(CMakePackageConfigHelpers)

install(TARGETS neustack_lib
    EXPORT NeuStackTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/neustack DESTINATION include)

# Generate CMake package config files for find_package(NeuStack)
install(EXPORT NeuStackTargets
    FILE NeuStackTargets.cmake
    NAMESPACE NeuStack::
    DESTINATION lib/cmake/NeuStack
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeuStackConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/NeuStackConfig.cmake"
    INSTALL_DESTINATION lib/cmake/NeuStack
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/NeuStackConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/NeuStackConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NeuStackConfigVersion.cmake"
    DESTINATION lib/cmake/NeuStack
)

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

# ============================================================================
# Tools
# ============================================================================
option(NEUSTACK_BUILD_TOOLS "Build CLI tools" ON)
if(NEUSTACK_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
