#ifndef NEUSTACK_AI_ONNX_INFERENCE_HPP
#define NEUSTACK_AI_ONNX_INFERENCE_HPP

// MinGW 兼容: 在 ONNX Runtime 头文件之前定义缺失的 SAL 注解和宏
#include "neustack/ai/onnxruntime_mingw_compat.hpp"

#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <optional>

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
    explicit ONNXInference(const std::string &model_path, int num_threads = 1);

    // 禁止拷贝
    ONNXInference(const ONNXInference &) = delete;
    ONNXInference &operator=(const ONNXInference &) = delete;

    // 允许移动
    ONNXInference(ONNXInference &&) = default;
    ONNXInference &operator=(ONNXInference &&) = default;

    ~ONNXInference() = default;

    // ─── 查询接口 ───

    // 获取输入名称
    const std::string &input_name() const { return _input_name; }

    // 获取输出名称
    const std::string &output_name() const { return _output_name; }

    // 获取输入形状
    const std::vector<int64_t> &input_shape() const { return _input_shape; }

    // 获取输出形状
    const std::vector<int64_t> &output_shape() const { return _output_shape; }

    // 获取输入元素数
    size_t input_size() const { return _input_size; }

    // 获取输出元素数
    size_t output_size() const { return _output_size; }

    /**
     * 获取模型自定义 metadata
     * @param key metadata 键名
     * @return 值，如果不存在返回 std::nullopt
     */
    std::optional<std::string> get_metadata(const std::string& key) const;

    // ─── 推理接口 ───

    /**
     * 执行推理（单个样本）
     * @param input 输入特征向量（长度=input_size()）
     * @return 输出向量（长度=output_size()）
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
    static Ort::Env &get_env(); // 全局 Env 实例

    std::unique_ptr<Ort::Session> _session;
    Ort::MemoryInfo _memory_info;

    // 元数据缓存
    std::string _input_name;
    std::string _output_name;
    std::vector<int64_t> _input_shape;
    std::vector<int64_t> _output_shape;
    size_t _input_size = 0;
    size_t _output_size = 0;

    // ─── 内部方法 ───
    void load_metadata();
    static size_t calc_size(const std::vector<int64_t> &shape);
};

} // namespace neustack


#endif // NEUSTACK_AI_ONNX_INFERENCE_HPP