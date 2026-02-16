#include "neustack/ai/onnx_inference.hpp"
#include "neustack/common/log.hpp"
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
namespace {
    std::wstring to_wide(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 0) {
            // fallback: basic ASCII conversion
            return std::wstring(s.begin(), s.end());
        }
        std::wstring ws(len - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
        return ws;
    }
}
#endif

using namespace neustack;

Ort::Env &ONNXInference::get_env() {
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
#ifdef _WIN32
        auto wide_path = to_wide(model_path);
        _session = std::make_unique<Ort::Session>(get_env(), wide_path.c_str(), options);
#else
        _session = std::make_unique<Ort::Session>(get_env(), model_path.c_str(), options);
#endif
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
size_t ONNXInference::calc_size(const std::vector<int64_t> &shape) {
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

    // 创建输入张量
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        _memory_info,
        const_cast<float *>(input.data()),  // CreateTensor 不会修改数据
        input.size(),
        input_shape.data(),
        input_shape.size()
    );

    // 准备输入/输出名称
    const char *input_names[] = {_input_name.c_str()};
    const char *output_names[] = {_output_name.c_str()};

    // 执行推理
    auto output_tensors = _session->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1
    );

    // 提取输出
    const float *output_data = output_tensors[0].GetTensorData<float>();
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

// ─── 获取模型 metadata ───
std::optional<std::string> ONNXInference::get_metadata(const std::string& key) const {
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        Ort::ModelMetadata metadata = _session->GetModelMetadata();

        // 获取所有 custom metadata keys
        auto keys = metadata.GetCustomMetadataMapKeysAllocated(allocator);

        for (size_t i = 0; i < keys.size(); i++) {
            if (std::string(keys[i].get()) == key) {
                auto value = metadata.LookupCustomMetadataMapAllocated(key.c_str(), allocator);
                return std::string(value.get());
            }
        }
    } catch (const Ort::Exception& e) {
        LOG_DEBUG(AI, "Failed to read metadata '%s': %s", key.c_str(), e.what());
    }

    return std::nullopt;
}