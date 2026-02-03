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
        const std::string &model_path,
        size_t history_length = 10,
        uint32_t max_bandwidth = 100 * 1024 * 1024 // 100 MB/s
    );

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char *name() const override { return "BandwidthPredictor"; }

    // IBandwidthModel 接口
    std::optional<Output> infer(const Input &input) override;
    size_t required_history_length() const override { return _history_length; }

private:
    std::unique_ptr<ONNXInference> _inference;
    size_t _history_length;
    uint32_t _max_bandwidth;
};

} // namespace neustack

#endif // NEUSTACK_AI_BANDWIDTH_MODEL_HPP