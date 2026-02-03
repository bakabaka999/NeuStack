#include "neustack/ai/orca_model.hpp"
#include "neustack/common/log.hpp"
#include <cmath>

using namespace neustack;

OrcaModel::OrcaModel(const std::string& model_path) {
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // 验证输入/输出维度
        if (_inference->input_size() != 7) {
            LOG_WARN(AI, "Orca model input size mismatch: expected 7, got %zu",
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

std::optional<ICongestionModel::Output> OrcaModel::infer(const Input &input) {
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
        input.in_flight_ratio,
        input.predicted_bw_normalized
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
    } catch (const std::exception &e) {
        LOG_ERROR(AI, "Orca inference failed: %s", e.what());
        return std::nullopt;
    }
}