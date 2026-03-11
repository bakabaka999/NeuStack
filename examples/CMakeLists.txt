# ============================================================================
# NeuStack Examples
# ============================================================================

# Minimal example (simplest usage)
add_executable(minimal minimal.cpp)
target_link_libraries(minimal PRIVATE neustack_lib)

# Firewall example
add_executable(fw_example fw_example.cpp)
target_link_libraries(fw_example PRIVATE neustack_lib)

# API Server example
add_executable(api_server api_server.cpp)
target_link_libraries(api_server PRIVATE neustack_lib)

# Ping example
add_executable(ping_example ping_example.cpp)
target_link_libraries(ping_example PRIVATE neustack_lib)

# TCP Echo Server example
add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE neustack_lib)

# DNS lookup example
add_executable(dns_example dns_example.cpp)
target_link_libraries(dns_example PRIVATE neustack_lib)

# HTTP Client example
add_executable(http_client http_client.cpp)
target_link_libraries(http_client PRIVATE neustack_lib)

# Telemetry/Prometheus example
add_executable(telemetry_example telemetry_example.cpp)
target_link_libraries(telemetry_example PRIVATE neustack_lib)

# UDP Echo Server example
add_executable(udp_example udp_example.cpp)
target_link_libraries(udp_example PRIVATE neustack_lib)

# Demo application (full-featured interactive demo)
add_executable(neustack_demo neustack_demo.cpp)
target_link_libraries(neustack_demo PRIVATE neustack_lib)

# RPATH for ONNX Runtime (demo links neustack_lib which may use ONNX)
if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    if(APPLE)
        set_target_properties(neustack_demo PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-arm64/lib;${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-x64/lib"
        )
    elseif(UNIX)
        set_target_properties(neustack_demo PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/linux-x64/lib"
        )
    endif()
endif()
