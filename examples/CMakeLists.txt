# ============================================================================
# NeuStack Examples
# ============================================================================

# Minimal example (simplest usage)
add_executable(minimal minimal.cpp)
target_link_libraries(minimal PRIVATE neustack_lib)

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
