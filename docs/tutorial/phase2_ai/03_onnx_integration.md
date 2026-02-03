# 教程 03：ONNX Runtime 集成

> **前置要求**: 完成教程 01-02 (双线程架构 + 指标采集)
> **目标**: 在 NeuStack 中集成 ONNX Runtime，实现高性能 AI 模型推理

---

## 1. 为什么选择 ONNX Runtime

### 1.1 推理引擎对比

| 引擎 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| **ONNX Runtime** | 跨框架、高性能、C++ 原生 | 模型需转换 | 生产部署 |
| PyTorch C++ (libtorch) | 与训练一致 | 体积大 (2GB+) | 研究原型 |
| TensorFlow Lite | 移动端优化 | API 复杂 | 嵌入式 |
| TensorRT | NVIDIA 最优 | 仅限 NVIDIA | GPU 推理 |

**选择 ONNX Runtime 的理由**：

1. **跨框架**: PyTorch、TensorFlow 训练的模型都能导出为 ONNX 格式
2. **轻量**: 核心库 ~20MB，远小于 libtorch
3. **高性能**: 自动优化计算图，支持多种 Execution Provider
4. **C++ 原生**: 无需 Python 依赖，适合嵌入协议栈

### 1.2 ONNX 生态系统

```
┌─────────────────────────────────────────────────────────────────┐
│                         训练阶段                                 │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐       │
│  │   PyTorch   │     │ TensorFlow  │     │    其他      │       │
│  │   训练代码   │     │   训练代码   │     │   框架      │       │
│  └──────┬──────┘     └──────┬──────┘     └──────┬──────┘       │
│         │                   │                   │               │
│         ▼                   ▼                   ▼               │
│  ┌─────────────────────────────────────────────────────┐       │
│  │              torch.onnx.export() / tf2onnx          │       │
│  └───────────────────────────┬─────────────────────────┘       │
│                              │                                  │
│                              ▼                                  │
│                    ┌─────────────────┐                         │
│                    │   model.onnx    │  ← 标准格式              │
│                    └─────────┬───────┘                         │
└──────────────────────────────┼──────────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────┼──────────────────────────────────┐
│                         推理阶段                                 │
│                    ┌─────────────────┐                         │
│                    │  ONNX Runtime   │                         │
│                    │   C++ API       │                         │
│                    └─────────────────┘                         │
│                              │                                  │
│           ┌──────────────────┼──────────────────┐              │
│           ▼                  ▼                  ▼              │
│     ┌──────────┐      ┌──────────┐       ┌──────────┐         │
│     │   CPU    │      │   CUDA   │       │  CoreML  │         │
│     │ Provider │      │ Provider │       │ Provider │         │
│     └──────────┘      └──────────┘       └──────────┘         │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 ONNX 文件格式

ONNX (Open Neural Network Exchange) 是一种开放的模型表示格式：

```protobuf
// model.onnx 的逻辑结构
ModelProto {
    ir_version: 8
    opset_import: [{domain: "", version: 17}]
    producer_name: "pytorch"

    GraphProto graph {
        // 输入定义
        input: [
            ValueInfoProto { name: "input", type: tensor(float, [1, 6]) }
        ]

        // 输出定义
        output: [
            ValueInfoProto { name: "output", type: tensor(float, [1, 1]) }
        ]

        // 计算节点
        node: [
            NodeProto { op_type: "MatMul", input: ["input", "weight"], output: ["fc1"] },
            NodeProto { op_type: "Relu", input: ["fc1"], output: ["relu1"] },
            // ...
        ]

        // 权重参数
        initializer: [
            TensorProto { name: "weight", dims: [6, 64], data_type: FLOAT, raw_data: ... }
        ]
    }
}
```

---

## 2. 安装与配置

### 2.1 下载 ONNX Runtime

项目提供了一键下载脚本，自动下载 macOS 和 Linux 的预编译库：

```bash
# 下载所有平台
./scripts/download_onnxruntime.sh

# 或指定平台
./scripts/download_onnxruntime.sh macos-arm64        # Apple Silicon
./scripts/download_onnxruntime.sh macos-x64           # Intel Mac
./scripts/download_onnxruntime.sh linux-x64           # Linux

# 下载后的目录结构
third_party/onnxruntime/
├── macos-arm64/                    # Apple Silicon
│   ├── include/
│   │   └── onnxruntime_cxx_api.h   # C++ API 主头文件
│   └── lib/
│       └── libonnxruntime.dylib     # 动态库
├── macos-x64/                      # Intel Mac
│   ├── include/
│   └── lib/
└── linux-x64/                      # Linux
    ├── include/
    └── lib/
        └── libonnxruntime.so

# 注意: third_party/ 已在 .gitignore 中，不会提交到 Git
```

### 2.2 项目结构

```
NeuStack/
├── third_party/
│   └── onnxruntime/                # 放置 ONNX Runtime
│       ├── include/
│       └── lib/
├── models/                          # ONNX 模型文件
│   ├── orca_actor.onnx
│   ├── anomaly_detector.onnx
│   └── bandwidth_predictor.onnx
├── include/neustack/ai/
│   ├── onnx_inference.hpp          # ONNX 推理封装
│   ├── orca_model.hpp              # Orca 拥塞控制
│   ├── anomaly_model.hpp           # 异常检测
│   └── bandwidth_model.hpp         # 带宽预测
└── src/ai/
    ├── onnx_inference.cpp
    ├── orca_model.cpp
    ├── anomaly_model.cpp
    └── bandwidth_model.cpp
```

### 2.3 CMake 配置

项目使用自定义的 `FindONNXRuntime.cmake` 模块自动检测平台并查找库：

```cmake
# cmake/FindONNXRuntime.cmake (核心逻辑)

# 平台检测
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(ONNXRUNTIME_PLATFORM "macos-arm64")
    else()
        set(ONNXRUNTIME_PLATFORM "macos-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.dylib")
elseif(UNIX)
    set(ONNXRUNTIME_PLATFORM "linux-x64")
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
endif()

# 设置搜索路径
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/${ONNXRUNTIME_PLATFORM}")

# 查找头文件和库
find_path(ONNXRUNTIME_INCLUDE_DIRS
    NAMES onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/include"
    PATH_SUFFIXES onnxruntime/core/session
    NO_DEFAULT_PATH
)

find_library(ONNXRUNTIME_LIBRARIES
    NAMES onnxruntime ${ONNXRUNTIME_LIB_NAME}
    PATHS "${ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH
)

# 设置结果变量
if(ONNXRUNTIME_INCLUDE_DIRS AND ONNXRUNTIME_LIBRARIES)
    set(ONNXRUNTIME_FOUND TRUE)
else()
    set(ONNXRUNTIME_FOUND FALSE)
endif()
```

**在 CMakeLists.txt 中使用**：

```cmake
# CMakeLists.txt

# 构建选项
option(NEUSTACK_ENABLE_AI "Enable AI congestion control (requires ONNX Runtime)" OFF)

# AI 源文件 (可选)
if(NEUSTACK_ENABLE_AI)
    # 使用自定义查找模块
    include(cmake/FindONNXRuntime.cmake)

    if(ONNXRUNTIME_FOUND)
        set(AI_SOURCES
            src/ai/onnx_inference.cpp
            # src/ai/orca_model.cpp
            # src/ai/anomaly_model.cpp
            # src/ai/bandwidth_model.cpp
        )
    else()
        message(WARNING "ONNX Runtime not found, AI features disabled")
        message(STATUS "  Run: ./scripts/download_onnxruntime.sh")
        set(NEUSTACK_ENABLE_AI OFF)
    endif()
endif()

# 主库
add_library(neustack_lib STATIC
    ${COMMON_SOURCES}
    ${NET_SOURCES}
    ${TRANSPORT_SOURCES}
    ${APP_SOURCES}
    ${METRICS_SOURCES}
    ${HAL_SOURCES}
    ${AI_SOURCES}
)

# 链接 ONNX Runtime
if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    target_include_directories(neustack_lib PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
    target_link_libraries(neustack_lib PRIVATE ${ONNXRUNTIME_LIBRARIES})
    target_compile_definitions(neustack_lib PUBLIC NEUSTACK_AI_ENABLED)

    # RPATH 设置 (确保运行时能找到动态库)
    if(APPLE)
        set_target_properties(neustack PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-arm64/lib;${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-x64/lib"
        )
    elseif(UNIX)
        set_target_properties(neustack PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/linux-x64/lib"
        )
    endif()
endif()
```

### 2.4 编译选项说明

```cmake
# 可选：启用特定的 Execution Provider
option(NEUSTACK_USE_COREML "Use CoreML EP on macOS" OFF)
option(NEUSTACK_USE_CUDA "Use CUDA EP on Linux" OFF)

# RPATH 设置（确保运行时能找到动态库）
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path/../lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()
```

---

## 3. ONNX Runtime C++ API 基础

### 3.1 核心概念

ONNX Runtime C++ API 采用 RAII 风格，核心对象包括：

| 对象 | 生命周期 | 说明 |
|------|----------|------|
| `Ort::Env` | 全局单例 | 运行时环境，管理线程池和日志 |
| `Ort::SessionOptions` | 配置时 | 会话配置（优化级别、EP 选择等）|
| `Ort::Session` | 模型级 | 加载的模型实例，可并发调用 |
| `Ort::MemoryInfo` | 常驻 | 描述内存位置（CPU/GPU）|
| `Ort::Value` | 推理时 | 输入/输出张量容器 |

```
┌─────────────────────────────────────────────────────────────────┐
│                        ONNX Runtime 架构                         │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                     Ort::Env (全局)                      │    │
│  │  • 线程池管理                                            │    │
│  │  • 日志配置                                              │    │
│  │  • 全局设置                                              │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Ort::SessionOptions                         │    │
│  │  • 优化级别 (ORT_ENABLE_ALL)                            │    │
│  │  • 执行提供者 (CPU, CUDA, CoreML)                       │    │
│  │  • 图优化配置                                            │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                  Ort::Session                            │    │
│  │  • 加载 ONNX 模型                                        │    │
│  │  • 执行推理 (线程安全)                                   │    │
│  │  • 查询输入/输出元数据                                   │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                   │
│                              ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │   Ort::Value (输入)          Ort::Value (输出)           │    │
│  │  ┌─────────────────┐        ┌─────────────────┐         │    │
│  │  │ float[1][6]     │   →    │ float[1][1]     │         │    │
│  │  │ 特征向量        │   Run  │ 预测结果         │         │    │
│  │  └─────────────────┘        └─────────────────┘         │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 最小示例

```cpp
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <vector>
#include <iostream>

int main() {
    // 1. 创建环境 (全局一次)
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "NeuStack");

    // 2. 配置会话
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);  // 单线程推理
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // 3. 加载模型
    const char* model_path = "models/orca_actor.onnx";
    Ort::Session session(env, model_path, session_options);

    // 4. 获取输入/输出信息
    Ort::AllocatorWithDefaultOptions allocator;

    // 输入名称
    auto input_name = session.GetInputNameAllocated(0, allocator);
    std::cout << "Input name: " << input_name.get() << std::endl;

    // 输入形状
    auto input_shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    std::cout << "Input shape: [";
    for (size_t i = 0; i < input_shape.size(); i++) {
        std::cout << input_shape[i] << (i < input_shape.size() - 1 ? ", " : "");
    }
    std::cout << "]" << std::endl;

    // 5. 准备输入数据
    std::vector<float> input_data = {0.5f, 0.3f, 1.2f, 0.01f, 0.8f, 0.7f};
    std::vector<int64_t> input_shape_vec = {1, 6};

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        input_data.data(),
        input_data.size(),
        input_shape_vec.data(),
        input_shape_vec.size()
    );

    // 6. 执行推理
    const char* input_names[] = {"input"};
    const char* output_names[] = {"output"};

    auto output_tensors = session.Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1
    );

    // 7. 获取输出
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    std::cout << "Output: " << output_data[0] << std::endl;

    return 0;
}
```

### 3.3 输入/输出张量详解

#### 创建输入张量

```cpp
// 方法 1: 从现有数据创建 (零拷贝)
// 注意: input_data 必须在推理期间保持有效!
std::vector<float> input_data = {1.0f, 2.0f, 3.0f};
std::vector<int64_t> shape = {1, 3};

Ort::Value tensor = Ort::Value::CreateTensor<float>(
    memory_info,
    input_data.data(),    // 数据指针
    input_data.size(),    // 元素数量
    shape.data(),         // 形状
    shape.size()          // 维度数
);

// 方法 2: 创建并填充 (有拷贝)
Ort::AllocatorWithDefaultOptions allocator;
Ort::Value tensor = Ort::Value::CreateTensor<float>(
    allocator, shape.data(), shape.size());
float* ptr = tensor.GetTensorMutableData<float>();
ptr[0] = 1.0f;
ptr[1] = 2.0f;
ptr[2] = 3.0f;
```

#### 读取输出张量

```cpp
auto output_tensors = session.Run(...);

// 获取第一个输出
Ort::Value& output = output_tensors[0];

// 检查类型
assert(output.IsTensor());

// 获取形状
auto shape_info = output.GetTensorTypeAndShapeInfo();
auto shape = shape_info.GetShape();        // e.g., {1, 1}
size_t total = shape_info.GetElementCount(); // e.g., 1

// 获取数据指针
const float* data = output.GetTensorData<float>();
// 或可变指针
float* mutable_data = output.GetTensorMutableData<float>();

// 复制到 vector
std::vector<float> result(data, data + total);
```

### 3.4 内存管理

#### MemoryInfo

`Ort::MemoryInfo` 描述张量数据的存储位置：

```cpp
// CPU 内存 (默认)
Ort::MemoryInfo cpu_memory = Ort::MemoryInfo::CreateCpu(
    OrtArenaAllocator,     // 分配器类型
    OrtMemTypeDefault      // 内存类型
);

// GPU 内存 (需要 CUDA EP)
// Ort::MemoryInfo gpu_memory("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
```

分配器类型：
- `OrtArenaAllocator`: 内存池分配，适合频繁分配释放
- `OrtDeviceAllocator`: 标准分配器

#### 避免内存泄漏

ONNX Runtime C++ API 使用 RAII，大部分对象自动管理：

```cpp
// 正确: RAII 自动释放
{
    Ort::Session session(env, "model.onnx", options);
    auto output = session.Run(...);
    // output 离开作用域时自动释放
}

// 错误: 原始 C API 需要手动释放
OrtSession* raw_session;
OrtCreateSession(env, "model.onnx", options, &raw_session);
// ... 使用 raw_session ...
OrtReleaseSession(raw_session);  // 必须手动释放!
```

### 3.5 线程安全

`Ort::Session::Run()` 是**线程安全**的：

```cpp
// 多线程并发推理
Ort::Session session(env, "model.onnx", options);

std::thread t1([&]() {
    auto result = session.Run(options, inputs1, ...);  // OK
});

std::thread t2([&]() {
    auto result = session.Run(options, inputs2, ...);  // OK
});
```

但要注意：
1. 每个线程应该有自己的输入/输出 `Ort::Value`
2. `Ort::Env` 应该全局共享

---

## 4. 封装推理接口

### 4.1 设计目标

我们需要一个通用的推理封装，满足：

1. **简单**: 上层代码不需要了解 ONNX Runtime 细节
2. **高效**: 避免重复分配，复用内存
3. **安全**: RAII 管理资源，异常安全
4. **灵活**: 支持不同形状的输入/输出

### 4.2 接口设计

```cpp
// include/neustack/ai/onnx_inference.hpp

#ifndef NEUSTACK_AI_ONNX_INFERENCE_HPP
#define NEUSTACK_AI_ONNX_INFERENCE_HPP

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <span>

namespace neustack {

/**
 * ONNX 模型推理器
 *
 * 线程安全: Run() 可以并发调用
 *
 * 用法:
 *   ONNXInference model("model.onnx");
 *   std::vector<float> input = {1.0, 2.0, 3.0};
 *   auto output = model.run(input);
 */
class ONNXInference {
public:
    /**
     * 构造并加载模型
     * @param model_path ONNX 模型文件路径
     * @param num_threads 推理线程数 (0 = 自动)
     * @throws Ort::Exception 加载失败时抛出
     */
    explicit ONNXInference(const std::string& model_path, int num_threads = 1);

    // 禁止拷贝
    ONNXInference(const ONNXInference&) = delete;
    ONNXInference& operator=(const ONNXInference&) = delete;

    // 允许移动
    ONNXInference(ONNXInference&&) = default;
    ONNXInference& operator=(ONNXInference&&) = default;

    ~ONNXInference() = default;

    // ─── 查询接口 ───

    /** 获取输入名称 */
    const std::string& input_name() const { return _input_name; }

    /** 获取输出名称 */
    const std::string& output_name() const { return _output_name; }

    /** 获取输入形状 (batch 维度可能为 -1 表示动态) */
    const std::vector<int64_t>& input_shape() const { return _input_shape; }

    /** 获取输出形状 */
    const std::vector<int64_t>& output_shape() const { return _output_shape; }

    /** 获取输入元素数 (不含 batch) */
    size_t input_size() const { return _input_size; }

    /** 获取输出元素数 (不含 batch) */
    size_t output_size() const { return _output_size; }

    // ─── 推理接口 ───

    /**
     * 执行推理 (单个样本)
     * @param input 输入特征向量 (长度 = input_size())
     * @return 输出向量 (长度 = output_size())
     */
    std::vector<float> run(std::span<const float> input);

    /**
     * 执行推理 (批量)
     * @param batch_input 批量输入 [batch_size * input_size]
     * @param batch_size 批量大小
     * @return 批量输出 [batch_size * output_size]
     */
    std::vector<float> run_batch(std::span<const float> batch_input, size_t batch_size);

    /**
     * 执行推理 (原地输出)
     * @param input 输入特征
     * @param output 输出缓冲区 (需预分配)
     */
    void run_inplace(std::span<const float> input, std::span<float> output);

private:
    // ─── ONNX Runtime 对象 ───
    static Ort::Env& get_env();  // 全局 Env 单例

    std::unique_ptr<Ort::Session> _session;
    Ort::MemoryInfo _memory_info;

    // ─── 元数据缓存 ───
    std::string _input_name;
    std::string _output_name;
    std::vector<int64_t> _input_shape;
    std::vector<int64_t> _output_shape;
    size_t _input_size = 0;
    size_t _output_size = 0;

    // ─── 内部方法 ───
    void load_metadata();
    static size_t calc_size(const std::vector<int64_t>& shape);
};

} // namespace neustack

#endif // NEUSTACK_AI_ONNX_INFERENCE_HPP
```

### 4.3 实现

```cpp
// src/ai/onnx_inference.cpp

#include "neustack/ai/onnx_inference.hpp"
#include "neustack/common/log.hpp"
#include <stdexcept>

namespace neustack {

// ─── 全局 Env 单例 ───
Ort::Env& ONNXInference::get_env() {
    // 线程安全的懒加载单例
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "NeuStack");
    return env;
}

// ─── 构造函数 ───
ONNXInference::ONNXInference(const std::string& model_path, int num_threads)
    : _memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    // 配置会话选项
    Ort::SessionOptions options;

    // 线程配置
    if (num_threads > 0) {
        options.SetIntraOpNumThreads(num_threads);
    }

    // 优化级别: 启用所有优化
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // 禁用内存模式匹配 (减少内存使用)
    // options.EnableMemPattern(false);

    // 加载模型
    try {
        _session = std::make_unique<Ort::Session>(get_env(), model_path.c_str(), options);
        LOG_INFO(AI, "Loaded ONNX model: %s", model_path.c_str());
    } catch (const Ort::Exception& e) {
        LOG_ERROR(AI, "Failed to load ONNX model '%s': %s", model_path.c_str(), e.what());
        throw;
    }

    // 加载元数据
    load_metadata();
}

// ─── 加载元数据 ───
void ONNXInference::load_metadata() {
    Ort::AllocatorWithDefaultOptions allocator;

    // 输入信息
    size_t num_inputs = _session->GetInputCount();
    if (num_inputs == 0) {
        throw std::runtime_error("Model has no inputs");
    }

    auto input_name_ptr = _session->GetInputNameAllocated(0, allocator);
    _input_name = input_name_ptr.get();

    auto input_info = _session->GetInputTypeInfo(0);
    auto input_tensor_info = input_info.GetTensorTypeAndShapeInfo();
    _input_shape = input_tensor_info.GetShape();
    _input_size = calc_size(_input_shape);

    // 输出信息
    size_t num_outputs = _session->GetOutputCount();
    if (num_outputs == 0) {
        throw std::runtime_error("Model has no outputs");
    }

    auto output_name_ptr = _session->GetOutputNameAllocated(0, allocator);
    _output_name = output_name_ptr.get();

    auto output_info = _session->GetOutputTypeInfo(0);
    auto output_tensor_info = output_info.GetTensorTypeAndShapeInfo();
    _output_shape = output_tensor_info.GetShape();
    _output_size = calc_size(_output_shape);

    LOG_DEBUG(AI, "Model input: %s [%zu], output: %s [%zu]",
              _input_name.c_str(), _input_size,
              _output_name.c_str(), _output_size);
}

// ─── 计算形状大小 ───
size_t ONNXInference::calc_size(const std::vector<int64_t>& shape) {
    size_t size = 1;
    for (int64_t dim : shape) {
        if (dim > 0) {
            size *= static_cast<size_t>(dim);
        }
        // dim == -1 表示动态维度，跳过
    }
    return size;
}

// ─── 单样本推理 ───
std::vector<float> ONNXInference::run(std::span<const float> input) {
    // 检查输入大小
    if (input.size() != _input_size) {
        throw std::invalid_argument("Input size mismatch: expected " +
                                    std::to_string(_input_size) +
                                    ", got " + std::to_string(input.size()));
    }

    // 构造输入形状 (batch = 1)
    std::vector<int64_t> input_shape = _input_shape;
    if (!input_shape.empty() && input_shape[0] <= 0) {
        input_shape[0] = 1;  // 动态 batch 设为 1
    }

    // 创建输入张量 (零拷贝)
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        _memory_info,
        const_cast<float*>(input.data()),  // CreateTensor 不会修改数据
        input.size(),
        input_shape.data(),
        input_shape.size()
    );

    // 准备输入/输出名称
    const char* input_names[] = {_input_name.c_str()};
    const char* output_names[] = {_output_name.c_str()};

    // 执行推理
    auto output_tensors = _session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1
    );

    // 提取输出
    const float* output_data = output_tensors[0].GetTensorData<float>();
    auto output_shape_info = output_tensors[0].GetTensorTypeAndShapeInfo();
    size_t output_count = output_shape_info.GetElementCount();

    return std::vector<float>(output_data, output_data + output_count);
}

// ─── 批量推理 ───
std::vector<float> ONNXInference::run_batch(std::span<const float> batch_input, size_t batch_size) {
    // 检查输入大小
    size_t expected_size = batch_size * _input_size;
    if (batch_input.size() != expected_size) {
        throw std::invalid_argument("Batch input size mismatch");
    }

    // 构造批量输入形状
    std::vector<int64_t> input_shape = _input_shape;
    if (!input_shape.empty()) {
        input_shape[0] = static_cast<int64_t>(batch_size);
    }

    // 创建输入张量
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        _memory_info,
        const_cast<float*>(batch_input.data()),
        batch_input.size(),
        input_shape.data(),
        input_shape.size()
    );

    // 准备输入/输出名称
    const char* input_names[] = {_input_name.c_str()};
    const char* output_names[] = {_output_name.c_str()};

    // 执行推理
    auto output_tensors = _session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1
    );

    // 提取输出
    const float* output_data = output_tensors[0].GetTensorData<float>();
    auto output_shape_info = output_tensors[0].GetTensorTypeAndShapeInfo();
    size_t output_count = output_shape_info.GetElementCount();

    return std::vector<float>(output_data, output_data + output_count);
}

// ─── 原地推理 ───
void ONNXInference::run_inplace(std::span<const float> input, std::span<float> output) {
    auto result = run(input);

    if (result.size() > output.size()) {
        throw std::invalid_argument("Output buffer too small");
    }

    std::copy(result.begin(), result.end(), output.begin());
}

} // namespace neustack
```

### 4.4 使用示例

```cpp
#include "neustack/ai/onnx_inference.hpp"
#include <iostream>

int main() {
    try {
        // 加载模型
        neustack::ONNXInference model("models/orca_actor.onnx");

        // 查看模型信息
        std::cout << "Input: " << model.input_name()
                  << " [" << model.input_size() << "]" << std::endl;
        std::cout << "Output: " << model.output_name()
                  << " [" << model.output_size() << "]" << std::endl;

        // 准备输入
        std::vector<float> features = {
            0.5f,   // throughput_normalized
            0.3f,   // queuing_delay_normalized
            1.2f,   // rtt_ratio
            0.01f,  // loss_rate
            0.8f,   // cwnd_normalized
            0.7f    // in_flight_ratio
        };

        // 推理
        auto output = model.run(features);

        std::cout << "Alpha: " << output[0] << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
```

---

## 5. 模型接口设计

### 5.1 模型基类

为三种 AI 模型定义统一接口：

```cpp
// include/neustack/ai/ai_model.hpp

#ifndef NEUSTACK_AI_AI_MODEL_HPP
#define NEUSTACK_AI_AI_MODEL_HPP

#include "neustack/ai/onnx_inference.hpp"
#include <optional>
#include <memory>

namespace neustack {

/**
 * AI 模型基类
 */
class AIModel {
public:
    virtual ~AIModel() = default;

    /** 模型是否已加载 */
    virtual bool is_loaded() const = 0;

    /** 获取模型名称 (用于日志) */
    virtual const char* name() const = 0;
};

/**
 * 拥塞控制模型 (Orca)
 *
 * 输入: OrcaFeatures (6 维)
 * 输出: alpha ∈ [-1, 1], cwnd_new = 2^alpha * cwnd_base
 */
class ICongestionModel : public AIModel {
public:
    struct Input {
        float throughput_normalized;
        float queuing_delay_normalized;
        float rtt_ratio;
        float loss_rate;
        float cwnd_normalized;
        float in_flight_ratio;
    };

    struct Output {
        float alpha;  // cwnd 调整因子
    };

    virtual std::optional<Output> infer(const Input& input) = 0;
};

/**
 * 异常检测模型 (LSTM-Autoencoder)
 *
 * 输入: AnomalyFeatures (5 维)
 * 输出: 重构误差，超过阈值则为异常
 */
class IAnomalyModel : public AIModel {
public:
    struct Input {
        float syn_rate;
        float rst_rate;
        float new_conn_rate;
        float packet_rate;
        float avg_packet_size;
    };

    struct Output {
        float reconstruction_error;
        bool is_anomaly;
    };

    virtual std::optional<Output> infer(const Input& input) = 0;

    /** 设置异常阈值 */
    virtual void set_threshold(float threshold) = 0;
};

/**
 * 带宽预测模型 (LSTM)
 *
 * 输入: 历史时序数据 (N 个时间步)
 * 输出: 预测带宽 (bytes/s)
 */
class IBandwidthModel : public AIModel {
public:
    struct Input {
        std::vector<float> throughput_history;
        std::vector<float> rtt_history;
        std::vector<float> loss_history;
    };

    struct Output {
        uint32_t predicted_bandwidth;  // bytes/s
        float confidence;              // [0, 1]
    };

    virtual std::optional<Output> infer(const Input& input) = 0;

    /** 获取所需的历史长度 */
    virtual size_t required_history_length() const = 0;
};

} // namespace neustack

#endif // NEUSTACK_AI_AI_MODEL_HPP
```

### 5.2 Orca 拥塞控制模型

```cpp
// include/neustack/ai/orca_model.hpp

#ifndef NEUSTACK_AI_ORCA_MODEL_HPP
#define NEUSTACK_AI_ORCA_MODEL_HPP

#include "neustack/ai/ai_model.hpp"
#include "neustack/ai/onnx_inference.hpp"
#include <memory>

namespace neustack {

/**
 * Orca 拥塞控制模型
 *
 * 基于 NSDI 2022 论文 "Orca: Pragmatic Learning-based Congestion Control"
 * 使用 DDPG 强化学习，输出 cwnd 调整因子
 */
class OrcaModel : public ICongestionModel {
public:
    /**
     * 构造 Orca 模型
     * @param model_path Actor 网络的 ONNX 模型路径
     */
    explicit OrcaModel(const std::string& model_path);

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char* name() const override { return "Orca"; }

    // ICongestionModel 接口
    std::optional<Output> infer(const Input& input) override;

private:
    std::unique_ptr<ONNXInference> _inference;
};

} // namespace neustack

#endif // NEUSTACK_AI_ORCA_MODEL_HPP
```

```cpp
// src/ai/orca_model.cpp

#include "neustack/ai/orca_model.hpp"
#include "neustack/common/log.hpp"
#include <cmath>

namespace neustack {

OrcaModel::OrcaModel(const std::string& model_path) {
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // 验证输入/输出维度
        if (_inference->input_size() != 6) {
            LOG_WARN(AI, "Orca model input size mismatch: expected 6, got %zu",
                     _inference->input_size());
        }
        if (_inference->output_size() != 1) {
            LOG_WARN(AI, "Orca model output size mismatch: expected 1, got %zu",
                     _inference->output_size());
        }

        LOG_INFO(AI, "Orca model loaded successfully");

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load Orca model: %s", e.what());
        _inference = nullptr;
    }
}

std::optional<ICongestionModel::Output> OrcaModel::infer(const Input& input) {
    if (!_inference) {
        return std::nullopt;
    }

    // 构造特征向量
    std::vector<float> features = {
        input.throughput_normalized,
        input.queuing_delay_normalized,
        input.rtt_ratio,
        input.loss_rate,
        input.cwnd_normalized,
        input.in_flight_ratio
    };

    try {
        auto result = _inference->run(features);

        if (result.empty()) {
            return std::nullopt;
        }

        // 输出 alpha ∈ [-1, 1] (tanh 激活)
        // 如果模型没有 tanh，手动 clamp
        float alpha = std::clamp(result[0], -1.0f, 1.0f);

        return Output{.alpha = alpha};

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Orca inference failed: %s", e.what());
        return std::nullopt;
    }
}

} // namespace neustack
```

### 5.3 异常检测模型

```cpp
// include/neustack/ai/anomaly_model.hpp

#ifndef NEUSTACK_AI_ANOMALY_MODEL_HPP
#define NEUSTACK_AI_ANOMALY_MODEL_HPP

#include "neustack/ai/ai_model.hpp"
#include "neustack/ai/onnx_inference.hpp"
#include <memory>

namespace neustack {

/**
 * LSTM-Autoencoder 异常检测模型
 *
 * 原理: 用正常流量训练自编码器，异常流量的重构误差会显著高于正常流量
 */
class AnomalyDetector : public IAnomalyModel {
public:
    /**
     * 构造异常检测器
     * @param model_path ONNX 模型路径
     * @param threshold 异常阈值 (重构误差大于此值判定为异常)
     */
    explicit AnomalyDetector(const std::string& model_path, float threshold = 0.5f);

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char* name() const override { return "AnomalyDetector"; }

    // IAnomalyModel 接口
    std::optional<Output> infer(const Input& input) override;
    void set_threshold(float threshold) override { _threshold = threshold; }

    /** 获取当前阈值 */
    float threshold() const { return _threshold; }

private:
    std::unique_ptr<ONNXInference> _inference;
    float _threshold;

    // 计算重构误差 (MSE)
    float compute_reconstruction_error(
        const std::vector<float>& input,
        const std::vector<float>& reconstructed
    );
};

} // namespace neustack

#endif // NEUSTACK_AI_ANOMALY_MODEL_HPP
```

```cpp
// src/ai/anomaly_model.cpp

#include "neustack/ai/anomaly_model.hpp"
#include "neustack/common/log.hpp"
#include <cmath>
#include <numeric>

namespace neustack {

AnomalyDetector::AnomalyDetector(const std::string& model_path, float threshold)
    : _threshold(threshold)
{
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // Autoencoder: 输入和输出维度应该相同
        if (_inference->input_size() != _inference->output_size()) {
            LOG_WARN(AI, "Autoencoder input/output size mismatch: %zu vs %zu",
                     _inference->input_size(), _inference->output_size());
        }

        LOG_INFO(AI, "Anomaly detector loaded, threshold=%.3f", _threshold);

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load anomaly model: %s", e.what());
        _inference = nullptr;
    }
}

std::optional<IAnomalyModel::Output> AnomalyDetector::infer(const Input& input) {
    if (!_inference) {
        return std::nullopt;
    }

    // 构造特征向量
    std::vector<float> features = {
        input.syn_rate,
        input.rst_rate,
        input.new_conn_rate,
        input.packet_rate,
        input.avg_packet_size
    };

    try {
        // 运行 Autoencoder
        auto reconstructed = _inference->run(features);

        // 计算重构误差
        float error = compute_reconstruction_error(features, reconstructed);

        // 判断是否异常
        bool anomaly = error > _threshold;

        if (anomaly) {
            LOG_WARN(AI, "Anomaly detected! reconstruction_error=%.4f (threshold=%.4f)",
                     error, _threshold);
        }

        return Output{
            .reconstruction_error = error,
            .is_anomaly = anomaly
        };

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Anomaly detection failed: %s", e.what());
        return std::nullopt;
    }
}

float AnomalyDetector::compute_reconstruction_error(
    const std::vector<float>& input,
    const std::vector<float>& reconstructed
) {
    if (input.size() != reconstructed.size()) {
        return std::numeric_limits<float>::max();
    }

    // Mean Squared Error
    float mse = 0.0f;
    for (size_t i = 0; i < input.size(); i++) {
        float diff = input[i] - reconstructed[i];
        mse += diff * diff;
    }
    mse /= static_cast<float>(input.size());

    return mse;
}

} // namespace neustack
```

### 5.4 带宽预测模型

```cpp
// include/neustack/ai/bandwidth_model.hpp

#ifndef NEUSTACK_AI_BANDWIDTH_MODEL_HPP
#define NEUSTACK_AI_BANDWIDTH_MODEL_HPP

#include "neustack/ai/ai_model.hpp"
#include "neustack/ai/onnx_inference.hpp"
#include <memory>

namespace neustack {

/**
 * LSTM 带宽预测模型
 *
 * 输入时序数据，预测未来的可用带宽
 */
class BandwidthPredictor : public IBandwidthModel {
public:
    /**
     * 构造带宽预测器
     * @param model_path ONNX 模型路径
     * @param history_length 所需的历史数据长度
     * @param max_bandwidth 归一化用的最大带宽 (bytes/s)
     */
    BandwidthPredictor(
        const std::string& model_path,
        size_t history_length = 10,
        uint32_t max_bandwidth = 100 * 1024 * 1024  // 100 MB/s
    );

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char* name() const override { return "BandwidthPredictor"; }

    // IBandwidthModel 接口
    std::optional<Output> infer(const Input& input) override;
    size_t required_history_length() const override { return _history_length; }

private:
    std::unique_ptr<ONNXInference> _inference;
    size_t _history_length;
    uint32_t _max_bandwidth;
};

} // namespace neustack

#endif // NEUSTACK_AI_BANDWIDTH_MODEL_HPP
```

```cpp
// src/ai/bandwidth_model.cpp

#include "neustack/ai/bandwidth_model.hpp"
#include "neustack/common/log.hpp"
#include <algorithm>

namespace neustack {

BandwidthPredictor::BandwidthPredictor(
    const std::string& model_path,
    size_t history_length,
    uint32_t max_bandwidth
)
    : _history_length(history_length)
    , _max_bandwidth(max_bandwidth)
{
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu, max_bw=%u",
                 _history_length, _max_bandwidth);

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load bandwidth model: %s", e.what());
        _inference = nullptr;
    }
}

std::optional<IBandwidthModel::Output> BandwidthPredictor::infer(const Input& input) {
    if (!_inference) {
        return std::nullopt;
    }

    // 检查历史数据长度
    if (input.throughput_history.size() < _history_length ||
        input.rtt_history.size() < _history_length ||
        input.loss_history.size() < _history_length) {
        LOG_DEBUG(AI, "Insufficient history for bandwidth prediction");
        return std::nullopt;
    }

    // 构造输入特征 (展平)
    // 格式: [throughput_0, ..., throughput_N, rtt_0, ..., rtt_N, loss_0, ..., loss_N]
    std::vector<float> features;
    features.reserve(_history_length * 3);

    // 取最近的 _history_length 个样本
    size_t start = input.throughput_history.size() - _history_length;

    for (size_t i = 0; i < _history_length; i++) {
        features.push_back(input.throughput_history[start + i]);
    }
    for (size_t i = 0; i < _history_length; i++) {
        features.push_back(input.rtt_history[start + i]);
    }
    for (size_t i = 0; i < _history_length; i++) {
        features.push_back(input.loss_history[start + i]);
    }

    try {
        auto result = _inference->run(features);

        if (result.empty()) {
            return std::nullopt;
        }

        // 输出是归一化的带宽 [0, 1]，反归一化
        float normalized_bw = std::clamp(result[0], 0.0f, 1.0f);
        uint32_t predicted_bw = static_cast<uint32_t>(normalized_bw * _max_bandwidth);

        // 置信度 (如果模型输出第二个值)
        float confidence = (result.size() > 1) ? std::clamp(result[1], 0.0f, 1.0f) : 0.5f;

        return Output{
            .predicted_bandwidth = predicted_bw,
            .confidence = confidence
        };

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Bandwidth prediction failed: %s", e.what());
        return std::nullopt;
    }
}

} // namespace neustack
```

---

## 6. 集成到智能面

### 6.1 智能面架构

智能面从数据面读取指标，运行 AI 模型推理，将决策发送回数据面：

```
┌────────────────────────────────────────────────────────────────┐
│                      Intelligence Plane                         │
│                                                                  │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────────┐   │
│  │ OrcaModel   │     │ Anomaly     │     │ Bandwidth       │   │
│  │ (10ms)      │     │ Detector    │     │ Predictor       │   │
│  │             │     │ (1s)        │     │ (100ms)         │   │
│  └──────┬──────┘     └──────┬──────┘     └───────┬─────────┘   │
│         │                   │                     │             │
│         └───────────────────┼─────────────────────┘             │
│                             ▼                                   │
│                    SPSCQueue<AIAction>                         │
│                             │                                   │
└─────────────────────────────┼───────────────────────────────────┘
                              ▼
                    ┌─────────────────┐
                    │   Data Plane    │
                    │  (TCP Layer)    │
                    └─────────────────┘
```

### 6.2 IntelligencePlane 头文件

```cpp
// include/neustack/ai/intelligence_plane.hpp

#ifndef NEUSTACK_AI_INTELLIGENCE_PLANE_HPP
#define NEUSTACK_AI_INTELLIGENCE_PLANE_HPP

#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/ai_action.hpp"
#include "neustack/metrics/ai_features.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/orca_model.hpp"
#include "neustack/ai/anomaly_model.hpp"
#include "neustack/ai/bandwidth_model.hpp"
#endif

#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

namespace neustack {

/**
 * 智能面配置
 *
 * 注意: 定义在类外面以避免 Apple Clang 的默认初始化器问题
 */
struct IntelligencePlaneConfig {
    // 模型路径 (空字符串 = 禁用该模型)
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;

    // 推理间隔
    std::chrono::milliseconds orca_interval{10};       // 10ms
    std::chrono::milliseconds anomaly_interval{1000};  // 1s
    std::chrono::milliseconds bandwidth_interval{100}; // 100ms

    // 异常检测阈值
    float anomaly_threshold = 0.5f;

    // 带宽预测历史长度
    size_t bandwidth_history_length = 10;
};

/**
 * 智能面 - AI 推理线程
 */
class IntelligencePlane {
public:
    using Config = IntelligencePlaneConfig;

    IntelligencePlane(
        MetricsBuffer<TCPSample, 1024>& metrics_buf,
        SPSCQueue<AIAction, 16>& action_queue,
        const Config& config = {}
    );

    ~IntelligencePlane();

    // 禁止拷贝和移动
    IntelligencePlane(const IntelligencePlane&) = delete;
    IntelligencePlane& operator=(const IntelligencePlane&) = delete;

    void start();
    void stop();
    bool is_running() const { return _running.load(std::memory_order_relaxed); }

private:
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    SPSCQueue<AIAction, 16>& _action_queue;

#ifdef NEUSTACK_AI_ENABLED
    std::unique_ptr<OrcaModel> _orca_model;
    std::unique_ptr<AnomalyDetector> _anomaly_model;
    std::unique_ptr<BandwidthPredictor> _bandwidth_model;
#endif

    Config _config;
    std::atomic<bool> _running{false};
    std::thread _thread;

    std::vector<TCPSample> _sample_history;
    GlobalMetrics::Snapshot _prev_snapshot;

    void run_loop();
    void process_orca();
    void process_anomaly(const GlobalMetrics::Snapshot::Delta& delta);
    void process_bandwidth();

    std::chrono::steady_clock::time_point _last_orca_time;
    std::chrono::steady_clock::time_point _last_anomaly_time;
    std::chrono::steady_clock::time_point _last_bandwidth_time;
};

} // namespace neustack

#endif // NEUSTACK_AI_INTELLIGENCE_PLANE_HPP
```

### 6.3 核心循环实现

```cpp
// src/ai/intelligence_plane.cpp (核心部分)

void IntelligencePlane::run_loop() {
    auto now = std::chrono::steady_clock::now();
    _last_orca_time = now;
    _last_anomaly_time = now;
    _last_bandwidth_time = now;

    size_t last_read_count = 0;

    while (_running.load(std::memory_order_relaxed)) {
        now = std::chrono::steady_clock::now();

        // ─── 1. 读取新的 TCPSample ───
        // MetricsBuffer 使用 recent() 而不是 try_pop()
        size_t current_count = _metrics_buf.total_pushed();
        if (current_count > last_read_count) {
            size_t new_samples = current_count - last_read_count;
            auto samples = _metrics_buf.recent(std::min(new_samples, size_t(100)));

            for (const auto& sample : samples) {
                _sample_history.push_back(sample);
            }

            // 限制历史长度
            constexpr size_t MAX_HISTORY = 1000;
            if (_sample_history.size() > MAX_HISTORY) {
                _sample_history.erase(
                    _sample_history.begin(),
                    _sample_history.begin() + (_sample_history.size() - MAX_HISTORY)
                );
            }

            last_read_count = current_count;
        }

        // ─── 2. 读取全局统计快照 ───
        auto snapshot = global_metrics().snapshot();
        auto delta = snapshot.diff(_prev_snapshot);
        _prev_snapshot = snapshot;

#ifdef NEUSTACK_AI_ENABLED
        // ─── 3. 按间隔执行各模型推理 ───

        if (_orca_model && _orca_model->is_loaded() &&
            now - _last_orca_time >= _config.orca_interval) {
            process_orca();
            _last_orca_time = now;
        }

        if (_anomaly_model && _anomaly_model->is_loaded() &&
            now - _last_anomaly_time >= _config.anomaly_interval) {
            process_anomaly(delta);
            _last_anomaly_time = now;
        }

        if (_bandwidth_model && _bandwidth_model->is_loaded() &&
            now - _last_bandwidth_time >= _config.bandwidth_interval) {
            process_bandwidth();
            _last_bandwidth_time = now;
        }
#else
        (void)delta;
#endif

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

### 6.4 Orca 推理

```cpp
void IntelligencePlane::process_orca() {
#ifdef NEUSTACK_AI_ENABLED
    if (_sample_history.empty()) return;

    const auto& sample = _sample_history.back();

    // 归一化参数
    constexpr float MAX_THROUGHPUT = 100e6f;  // 100 Mbps
    constexpr float MAX_DELAY = 100000.0f;    // 100ms in us
    constexpr float MAX_CWND = 1000.0f;       // 1000 MSS

    ICongestionModel::Input input{
        .throughput_normalized = static_cast<float>(sample.delivery_rate) / MAX_THROUGHPUT,
        .queuing_delay_normalized = static_cast<float>(sample.queuing_delay_us()) / MAX_DELAY,
        .rtt_ratio = sample.rtt_ratio(),
        .loss_rate = sample.loss_rate(),
        .cwnd_normalized = static_cast<float>(sample.cwnd) / MAX_CWND,
        .in_flight_ratio = (sample.cwnd > 0)
            ? static_cast<float>(sample.bytes_in_flight) / (sample.cwnd * 1460)
            : 0.0f
    };

    auto result = _orca_model->infer(input);
    if (result) {
        AIAction action{};
        action.type = AIAction::Type::CWND_ADJUST;
        action.conn_id = 0;
        action.cwnd.alpha = result->alpha;

        _action_queue.try_push(action);
    }
#endif
}
```

### 6.5 异常检测推理

```cpp
void IntelligencePlane::process_anomaly(const GlobalMetrics::Snapshot::Delta& delta) {
#ifdef NEUSTACK_AI_ENABLED
    float interval_sec = static_cast<float>(_config.anomaly_interval.count()) / 1000.0f;
    if (interval_sec <= 0) interval_sec = 1.0f;

    constexpr float MAX_RATE = 10000.0f;
    constexpr float MAX_PACKET_SIZE = 1500.0f;

    float avg_packet_size = (delta.packets_rx > 0)
        ? static_cast<float>(delta.bytes_rx) / delta.packets_rx
        : 0.0f;

    IAnomalyModel::Input input{
        .syn_rate = static_cast<float>(delta.syn_received) / interval_sec / MAX_RATE,
        .rst_rate = static_cast<float>(delta.rst_received) / interval_sec / MAX_RATE,
        .new_conn_rate = static_cast<float>(delta.conn_established) / interval_sec / MAX_RATE,
        .packet_rate = static_cast<float>(delta.packets_rx) / interval_sec / MAX_RATE,
        .avg_packet_size = avg_packet_size / MAX_PACKET_SIZE
    };

    auto result = _anomaly_model->infer(input);
    if (result && result->is_anomaly) {
        AIAction action{};
        action.type = AIAction::Type::ANOMALY_ALERT;
        action.conn_id = 0;
        action.anomaly.score = result->reconstruction_error;

        _action_queue.try_push(action);
        LOG_WARN(AI, "Anomaly detected! score=%.4f", result->reconstruction_error);
    }
#else
    (void)delta;
#endif
}
```

### 6.6 带宽预测推理

```cpp
void IntelligencePlane::process_bandwidth() {
#ifdef NEUSTACK_AI_ENABLED
    size_t required = _bandwidth_model->required_history_length();
    if (_sample_history.size() < required) {
        return;
    }

    IBandwidthModel::Input input;
    input.throughput_history.reserve(required);
    input.rtt_history.reserve(required);
    input.loss_history.reserve(required);

    constexpr float MAX_THROUGHPUT = 100e6f;
    constexpr float MAX_RTT = 100000.0f;

    size_t start = _sample_history.size() - required;
    for (size_t i = 0; i < required; i++) {
        const auto& sample = _sample_history[start + i];
        input.throughput_history.push_back(
            static_cast<float>(sample.delivery_rate) / MAX_THROUGHPUT
        );
        input.rtt_history.push_back(
            static_cast<float>(sample.rtt_us) / MAX_RTT
        );
        input.loss_history.push_back(sample.loss_rate());
    }

    auto result = _bandwidth_model->infer(input);
    if (result) {
        AIAction action{};
        action.type = AIAction::Type::BW_PREDICTION;
        action.conn_id = 0;
        action.bandwidth.predicted_bw = result->predicted_bandwidth;

        _action_queue.try_push(action);
    }
#endif
}
```

### 6.7 使用示例

```cpp
// 在 main.cpp 或 tcp_layer.cpp 中

MetricsBuffer<TCPSample, 1024> metrics_buf;
SPSCQueue<AIAction, 16> action_queue;

IntelligencePlane::Config config;
config.orca_model_path = "models/orca_actor.onnx";
config.anomaly_model_path = "models/anomaly_detector.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";

IntelligencePlane intel_plane(metrics_buf, action_queue, config);
intel_plane.start();

// ... 运行中 ...

intel_plane.stop();
```

---

## 7. 性能优化

### 7.1 推理延迟分析

| 模型 | 输入维度 | 典型延迟 (CPU) | 频率 |
|------|----------|----------------|------|
| Orca Actor | 6 | < 0.1 ms | 每 10ms |
| Anomaly AE | 5 | < 0.5 ms | 每 1s |
| Bandwidth LSTM | 30 | < 1 ms | 每 100ms |

### 7.2 优化技巧

#### 1. 减少内存分配

```cpp
// 不好: 每次推理都分配
std::vector<float> features = {...};
auto result = model.run(features);

// 好: 复用缓冲区
thread_local std::vector<float> features_buf(6);
features_buf[0] = input.throughput_normalized;
// ...
auto result = model.run(features_buf);
```

#### 2. 批量推理

如果有多个连接需要推理，可以批量处理：

```cpp
// 单个推理 x N
for (auto& tcb : connections) {
    model.run(tcb->features);  // N 次推理
}

// 批量推理 (更高效)
std::vector<float> batch_input;
for (auto& tcb : connections) {
    batch_input.insert(batch_input.end(),
                       tcb->features.begin(), tcb->features.end());
}
model.run_batch(batch_input, connections.size());  // 1 次推理
```

#### 3. 模型优化

```cpp
// 启用所有图优化
session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

// 减少线程 (单线程推理避免调度开销)
session_options.SetIntraOpNumThreads(1);

// 使用执行提供者
#ifdef __APPLE__
    // CoreML (Apple 芯片加速)
    session_options.AppendExecutionProvider_CoreML();
#endif
```

#### 4. 模型量化

训练时导出 INT8 量化模型：

```python
# PyTorch 动态量化
model_quantized = torch.quantization.quantize_dynamic(
    model, {torch.nn.Linear}, dtype=torch.qint8
)
torch.onnx.export(model_quantized, ...)
```

### 7.3 延迟监控

```cpp
// 添加推理延迟监控
class InferenceLatencyMonitor {
public:
    void record(std::chrono::microseconds latency) {
        _total_us += latency.count();
        _count++;
        _max_us = std::max(_max_us, static_cast<uint64_t>(latency.count()));
    }

    double avg_us() const { return _count > 0 ? double(_total_us) / _count : 0; }
    uint64_t max_us() const { return _max_us; }

private:
    std::atomic<uint64_t> _total_us{0};
    std::atomic<uint64_t> _count{0};
    std::atomic<uint64_t> _max_us{0};
};

// 使用
auto start = std::chrono::steady_clock::now();
auto result = model.run(input);
auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - start
);
latency_monitor.record(elapsed);
```

---

## 8. 模型导出指南

### 8.1 PyTorch → ONNX

```python
import torch
import torch.nn as nn

# 定义 Orca Actor 网络
class OrcaActor(nn.Module):
    def __init__(self, input_dim=6, hidden_dim=64):
        super().__init__()
        self.fc1 = nn.Linear(input_dim, hidden_dim)
        self.fc2 = nn.Linear(hidden_dim, hidden_dim)
        self.fc3 = nn.Linear(hidden_dim, 1)

    def forward(self, x):
        x = torch.relu(self.fc1(x))
        x = torch.relu(self.fc2(x))
        x = torch.tanh(self.fc3(x))  # 输出 [-1, 1]
        return x

# 创建模型
model = OrcaActor()
model.eval()

# 导出 ONNX
dummy_input = torch.randn(1, 6)  # batch=1, features=6
torch.onnx.export(
    model,
    dummy_input,
    "orca_actor.onnx",
    input_names=["input"],
    output_names=["output"],
    dynamic_axes={
        "input": {0: "batch_size"},
        "output": {0: "batch_size"}
    },
    opset_version=17
)

print("Exported to orca_actor.onnx")
```

### 8.2 验证导出的模型

```python
import onnx
import onnxruntime as ort
import numpy as np

# 加载并验证
model = onnx.load("orca_actor.onnx")
onnx.checker.check_model(model)
print("Model is valid!")

# 测试推理
session = ort.InferenceSession("orca_actor.onnx")
input_data = np.random.randn(1, 6).astype(np.float32)
output = session.run(None, {"input": input_data})
print(f"Input shape: {input_data.shape}")
print(f"Output shape: {output[0].shape}")
print(f"Output value: {output[0]}")
```

### 8.3 模型文件组织

```
models/
├── orca_actor.onnx         # Orca 拥塞控制 (训练后)
├── anomaly_detector.onnx   # 异常检测 (训练后)
├── bandwidth_predictor.onnx # 带宽预测 (训练后)
└── README.md               # 模型说明文档
```

---

## 9. 完整 CMake 配置

项目实际使用分离的查找模块，以下是简化的完整配置示例：

```cmake
# CMakeLists.txt (简化版，实际项目参见完整的 CMakeLists.txt)

cmake_minimum_required(VERSION 3.20)
project(NeuStack VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ═══════════════════════════════════════════════════════════════════
# 构建选项
# ═══════════════════════════════════════════════════════════════════

option(NEUSTACK_ENABLE_AI "Enable AI congestion control (requires ONNX Runtime)" OFF)

# ═══════════════════════════════════════════════════════════════════
# 平台检测
# ═══════════════════════════════════════════════════════════════════

include(cmake/Platform.cmake)

# ═══════════════════════════════════════════════════════════════════
# 源文件
# ═══════════════════════════════════════════════════════════════════

set(COMMON_SOURCES src/common/checksum.cpp src/common/ip_addr.cpp)
set(NET_SOURCES src/net/ipv4.cpp src/net/icmp.cpp)
set(TRANSPORT_SOURCES
    src/transport/udp.cpp
    src/transport/tcp_segment.cpp
    src/transport/tcp_builder.cpp
    src/transport/tcp_connection.cpp
    src/transport/tcp_layer.cpp
)
set(APP_SOURCES
    src/app/http_types.cpp
    src/app/http_parser.cpp
    src/app/http_server.cpp
    src/app/http_client.cpp
    src/app/dns_client.cpp
)
set(METRICS_SOURCES src/metrics/ai_features.cpp)

# 平台特定 HAL
if(NEUSTACK_PLATFORM_MACOS)
    set(HAL_SOURCES src/hal/device.cpp src/hal/hal_macos.cpp)
elseif(NEUSTACK_PLATFORM_LINUX)
    set(HAL_SOURCES src/hal/device.cpp src/hal/hal_linux.cpp)
endif()

# AI 源文件 (可选)
if(NEUSTACK_ENABLE_AI)
    include(cmake/FindONNXRuntime.cmake)

    if(ONNXRUNTIME_FOUND)
        set(AI_SOURCES
            src/ai/onnx_inference.cpp
            # 后续教程添加:
            # src/ai/orca_model.cpp
            # src/ai/anomaly_model.cpp
            # src/ai/bandwidth_model.cpp
        )
    else()
        message(WARNING "ONNX Runtime not found, AI features disabled")
        message(STATUS "  Run: ./scripts/download_onnxruntime.sh")
        set(NEUSTACK_ENABLE_AI OFF)
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════
# 库目标
# ═══════════════════════════════════════════════════════════════════

add_library(neustack_lib STATIC
    ${COMMON_SOURCES}
    ${NET_SOURCES}
    ${TRANSPORT_SOURCES}
    ${APP_SOURCES}
    ${METRICS_SOURCES}
    ${HAL_SOURCES}
    ${AI_SOURCES}
)

target_include_directories(neustack_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# 平台特定库
if(NEUSTACK_PLATFORM_MACOS)
    target_link_libraries(neustack_lib PRIVATE
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
    )
endif()

# ONNX Runtime 链接
if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    target_include_directories(neustack_lib PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
    target_link_libraries(neustack_lib PRIVATE ${ONNXRUNTIME_LIBRARIES})
    target_compile_definitions(neustack_lib PUBLIC NEUSTACK_AI_ENABLED)

    # RPATH 设置 (确保运行时能找到动态库)
    if(APPLE)
        set_target_properties(neustack PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-arm64/lib;${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-x64/lib"
        )
    elseif(UNIX)
        set_target_properties(neustack PROPERTIES
            BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/linux-x64/lib"
        )
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════
# 可执行文件
# ═══════════════════════════════════════════════════════════════════

add_executable(neustack src/main.cpp)
target_link_libraries(neustack PRIVATE neustack_lib)

# ═══════════════════════════════════════════════════════════════════
# 构建摘要
# ═══════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "========== NeuStack Build Configuration ==========")
message(STATUS "Platform:         ${NEUSTACK_PLATFORM_NAME}")
message(STATUS "Enable AI:        ${NEUSTACK_ENABLE_AI}")
if(NEUSTACK_ENABLE_AI AND ONNXRUNTIME_FOUND)
    message(STATUS "ONNX Runtime:     ${ONNXRUNTIME_ROOT}")
endif()
message(STATUS "==================================================")
message(STATUS "")
```

**启用 AI 功能构建**：

```bash
# 1. 下载 ONNX Runtime
./scripts/download_onnxruntime.sh

# 2. 配置并构建 (启用 AI)
mkdir -p build && cd build
cmake .. -DNEUSTACK_ENABLE_AI=ON
make -j$(nproc)
```

---

## 10. 新增文件清单

```
include/neustack/ai/
├── onnx_inference.hpp      # ONNX Runtime 封装
├── ai_model.hpp            # 模型接口基类
├── orca_model.hpp          # Orca 拥塞控制模型
├── anomaly_model.hpp       # 异常检测模型
├── bandwidth_model.hpp     # 带宽预测模型
└── intelligence_plane.hpp  # 智能面 (更新)

src/ai/
├── onnx_inference.cpp
├── orca_model.cpp
├── anomaly_model.cpp
├── bandwidth_model.cpp
└── intelligence_plane.cpp

third_party/
└── onnxruntime/            # ONNX Runtime 库
    ├── include/
    └── lib/

models/                      # ONNX 模型文件
├── orca_actor.onnx
├── anomaly_detector.onnx
└── bandwidth_predictor.onnx
```

---

## 11. 下一步

- **教程 04: 异常检测** — 训练 LSTM-Autoencoder，部署检测
- **教程 05: Orca 拥塞控制** — DDPG 训练，与 TCP Cubic 集成
- **教程 06: 带宽预测** — LSTM 时序预测，应用层 API

---

## 12. 参考资料

### ONNX Runtime
- [官方文档](https://onnxruntime.ai/docs/)
- [C++ API 参考](https://onnxruntime.ai/docs/api/c/index.html)
- [GitHub 仓库](https://github.com/microsoft/onnxruntime)

### ONNX 格式
- [ONNX 规范](https://onnx.ai/onnx/intro/)
- [算子列表](https://onnx.ai/onnx/operators/)

### 模型导出
- [PyTorch ONNX 导出](https://pytorch.org/docs/stable/onnx.html)
- [TensorFlow to ONNX](https://github.com/onnx/tensorflow-onnx)

### 论文
- [Orca: Pragmatic Learning-based Congestion Control](https://www.usenix.org/conference/nsdi22/presentation/abbasloo)
- [LSTM-Autoencoder for Intrusion Detection](https://arxiv.org/abs/2204.03779)
