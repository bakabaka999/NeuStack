# ============================================================================
# NeuStack Examples
# ============================================================================

# Echo server example (requires TCP implementation)
# add_executable(echo_server echo_server.cpp)
# target_link_libraries(echo_server PRIVATE neustack_lib)

# Ping example (requires ICMP implementation)
# add_executable(ping_example ping.cpp)
# target_link_libraries(ping_example PRIVATE neustack_lib)

# UDP example
# add_executable(udp_example udp_echo.cpp)
# target_link_libraries(udp_example PRIVATE neustack_lib)

# ONNX Runtime test (requires ONNX Runtime)
if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    add_executable(onnx_test onnx_test.cpp)
    target_include_directories(onnx_test PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
    target_link_libraries(onnx_test PRIVATE ${ONNXRUNTIME_LIBRARIES})

    # AI Integration test (full pipeline)
    add_executable(ai_test ai_test.cpp)
    target_link_libraries(ai_test PRIVATE neustack_lib)

    # RPATH for dynamic library
    if(APPLE)
        set_target_properties(onnx_test ai_test PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-arm64/lib;${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-x64/lib"
        )
    elseif(UNIX)
        set_target_properties(onnx_test ai_test PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/linux-x64/lib"
        )
    endif()
endif()
