# FindONNXRuntime.cmake
#
# 根据当前平台自动选择对应的 ONNX Runtime 预编译库
#
# 设置以下变量:
#   ONNXRUNTIME_FOUND        - 是否找到
#   ONNXRUNTIME_INCLUDE_DIRS - 头文件路径
#   ONNXRUNTIME_LIBRARIES    - 库文件路径
#   ONNXRUNTIME_PLATFORM     - 当前平台标识

set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")

# ─── 检测平台 ───
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(ONNXRUNTIME_PLATFORM "macos-arm64")
    else()
        set(ONNXRUNTIME_PLATFORM "macos-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.dylib")
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(ONNXRUNTIME_PLATFORM "linux-aarch64")
    else()
        set(ONNXRUNTIME_PLATFORM "linux-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
        set(ONNXRUNTIME_PLATFORM "win-arm64")
    else()
        set(ONNXRUNTIME_PLATFORM "win-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "onnxruntime.lib")
else()
    message(WARNING "Unsupported platform for ONNX Runtime")
    set(ONNXRUNTIME_FOUND FALSE)
    return()
endif()

set(ONNXRUNTIME_DIR "${ONNXRUNTIME_ROOT}/${ONNXRUNTIME_PLATFORM}")

# ─── 查找头文件和库 ───
# ONNX Runtime 的头文件可能在 include/ 或 include/onnxruntime/ 下
find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS
        "${ONNXRUNTIME_DIR}/include"
        "${ONNXRUNTIME_DIR}/include/onnxruntime"
        "${ONNXRUNTIME_DIR}/include/onnxruntime/core/session"
    NO_DEFAULT_PATH
)

find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    PATHS "${ONNXRUNTIME_DIR}/lib"
    NO_DEFAULT_PATH
)

# ─── 设置结果变量 ───
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    set(ONNXRUNTIME_FOUND TRUE)
    set(ONNXRUNTIME_INCLUDE_DIRS "${ONNXRUNTIME_INCLUDE_DIR}")
    set(ONNXRUNTIME_LIBRARIES "${ONNXRUNTIME_LIBRARY}")

    message(STATUS "Found ONNX Runtime [${ONNXRUNTIME_PLATFORM}]")
    message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIRS}")
    message(STATUS "  Library: ${ONNXRUNTIME_LIBRARIES}")
else()
    set(ONNXRUNTIME_FOUND FALSE)
    message(STATUS "ONNX Runtime not found for platform: ${ONNXRUNTIME_PLATFORM}")
    message(STATUS "  Expected: ${ONNXRUNTIME_DIR}")
    message(STATUS "  Run: ./scripts/download/download_onnxruntime.sh ${ONNXRUNTIME_PLATFORM}")
endif()
