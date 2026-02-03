#include "neustack/ai/bandwidth_model.hpp"
#include "neustack/common/log.hpp"
#include <algorithm>

using namespace neustack;

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

        // 尝试从模型 metadata 读取 history_length
        auto hl_str = _inference->get_metadata("history_length");
        if (hl_str) {
            try {
                _history_length = std::stoul(*hl_str);
                LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu (from metadata), max_bw=%u",
                         _history_length, _max_bandwidth);
            } catch (...) {
                LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu (from parameter), max_bw=%u",
                         _history_length, _max_bandwidth);
            }
        } else {
            LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu, max_bw=%u",
                     _history_length, _max_bandwidth);
        }

    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load bandwidth model: %s", e.what());
        _inference = nullptr;
    }
}

std::optional<IBandwidthModel::Output> BandwidthPredictor::infer(const Input &input) {
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

    // 构造输入特征（展平）
    // 格式: [t0, r0, l0, t1, r1, l1, ..., tN, rN, lN] (按时间步分组)
    // LSTM 期望 [batch, seq_len, features]，展平后是 seq_len * features
    std::vector<float> features;
    features.reserve(_history_length * 3);

    // 取最近的 _history_length 样本
    size_t start = input.throughput_history.size() - _history_length;

    for (size_t i = 0; i < _history_length; i++) {
        features.push_back(input.throughput_history[start + i]);
        features.push_back(input.rtt_history[start + i]);
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