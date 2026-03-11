#include "neustack/ai/anomaly_model.hpp"
#include "neustack/common/log.hpp"
#include <cmath>
#include <numeric>

using namespace neustack;

AnomalyDetector::AnomalyDetector(const std::string &model_path, float threshold) : _threshold(threshold) {
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // Autoencoder: 输入和输出维度应当相同
        if (_inference->input_size() != _inference->output_size()) {
            LOG_WARN(AI, "Autoencoder input/output size mismatch: %zu vs %zu",
                     _inference->input_size(), _inference->output_size());
        }

        // 尝试从模型 metadata 读取阈值
        auto threshold_str = _inference->get_metadata("anomaly_threshold");
        if (threshold_str) {
            try {
                _threshold = std::stof(*threshold_str);
                LOG_INFO(AI, "Anomaly detector loaded, threshold=%.6f (from model metadata)", _threshold);
            } catch (const std::exception&) {
                LOG_WARN(AI, "Invalid threshold in model metadata: %s", threshold_str->c_str());
                LOG_INFO(AI, "Anomaly detector loaded, threshold=%.6f (from parameter)", _threshold);
            }
        } else {
            LOG_INFO(AI, "Anomaly detector loaded, threshold=%.6f (from parameter)", _threshold);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(AI, "Failed to load anomaly model: %s", e.what());
        _inference = nullptr;
    }
}

std::optional<IAnomalyModel::Output> AnomalyDetector::infer(const Input &input) {
    if (!_inference) {
        return std::nullopt;
    }

    // 构造特征向量 (8维)
    std::vector<float> features = {
        input.log_pkt_rate,
        input.bytes_per_pkt,
        input.syn_ratio,
        input.rst_ratio,
        input.conn_completion,
        input.tx_rx_ratio,
        input.log_active_conn,
        input.log_conn_reset
    };

    try {
        // 执行推理（Autoencoder 重构）
        auto reconstructed = _inference->run(features);

        // 计算重构误差
        float error = compute_reconstruction_error(features, reconstructed);

        return Output{
            .reconstruction_error = error,
            .is_anomaly = (error > _threshold)
        };
    } catch (const std::exception &e) {
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
