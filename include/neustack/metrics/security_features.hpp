#ifndef NEUSTACK_METRICS_SECURITY_FEATURES_HPP
#define NEUSTACK_METRICS_SECURITY_FEATURES_HPP

#include "neustack/metrics/security_metrics.hpp"
#include "neustack/ai/ai_model.hpp"

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace neustack {

/**
 * AI 异常检测输入特征
 *
 * 独立定义，避免依赖 ai_model.hpp（需要 ONNX Runtime）
 * 与 AnomalyInput 兼容
 */
struct AnomalyInput {
    float packets_rx_norm;
    float packets_tx_norm;
    float bytes_tx_norm;
    float syn_rate_norm;
    float rst_rate_norm;
    float conn_established_norm;
    float tx_rx_ratio_norm;
    float active_conn_norm;
};

/**
 * 安全特征提取器 - 将原始指标转换为 AI 模型输入
 *
 * 设计原则:
 * - 所有特征归一化到 [0, 1] 区间
 * - 使用领域知识设定归一化上界
 * - 提供多种归一化策略（线性、对数、sigmoid）
 *
 * 特征说明:
 * 1. packets_rx_norm   - 包速率（归一化）
 * 2. packets_tx_norm   - 发送包速率（归一化）
 * 3. bytes_tx_norm     - 字节速率（归一化）
 * 4. syn_rate_norm     - SYN 包速率（归一化），高值可能是 SYN Flood
 * 5. rst_rate_norm     - RST 包速率（归一化），高值可能是端口扫描
 * 6. conn_established_norm - 新连接速率（归一化）
 * 7. tx_rx_ratio_norm  - 发送/接收比率（归一化）
 * 8. active_conn_norm  - 活跃连接数（归一化）
 *
 * 异常检测原理:
 * - Autoencoder 用正常流量训练，学习"正常"的特征分布
 * - 异常流量的特征分布与正常流量不同，重构误差会升高
 * - 重构误差超过阈值即判定为异常
 */
class SecurityFeatureExtractor {
public:
    // ─── 归一化参数（可根据网络环境调整）───
    struct NormalizationParams {
        // 速率上界（用于线性归一化）
        double max_pps;
        double max_bps;
        double max_syn_rate;
        double max_rst_rate;
        double max_conn_rate;
        double max_active_conn;
        
        // SYN/SYN-ACK 比率阈值（正常约 1.0，SYN Flood 时会很高）
        double syn_ratio_warning;
        double syn_ratio_critical;
        
        NormalizationParams()
            : max_pps(20000.0)
            , max_bps(20000000.0)
            , max_syn_rate(500.0)
            , max_rst_rate(100.0)
            , max_conn_rate(500.0)
            , max_active_conn(10000.0)
            , syn_ratio_warning(50.0)
            , syn_ratio_critical(200.0)
        {}
    };

    explicit SecurityFeatureExtractor(const NormalizationParams& params = NormalizationParams{})
        : _params(params) {}

    /**
     * 从 SecurityMetrics 快照提取特征
     *
     * @param snapshot 安全指标快照
     * @param active_connections 当前活跃连接数（从 GlobalMetrics 获取）
     * @return AnomalyInput 格式的特征向量
     */
    AnomalyInput extract(const SecurityMetrics::Snapshot& snapshot,
                                  uint32_t active_connections = 0) const {
        AnomalyInput features{};
        
        // 1. 包速率（线性归一化）
        features.packets_rx_norm = normalize_linear(snapshot.pps, _params.max_pps);
        
        // 2. 发送包速率（这里用相同值，因为 SecurityMetrics 目前只统计 RX）
        //    如果需要区分，可以扩展 SecurityMetrics
        features.packets_tx_norm = features.packets_rx_norm;
        
        // 3. 字节速率（对数归一化，因为范围很大）
        features.bytes_tx_norm = normalize_log(snapshot.bps, _params.max_bps);
        
        // 4. SYN 速率（线性归一化）
        //    高 SYN 速率是 SYN Flood 的特征
        features.syn_rate_norm = normalize_linear(snapshot.syn_rate, _params.max_syn_rate);
        
        // 5. RST 速率（线性归一化）
        //    高 RST 速率可能是端口扫描的特征
        features.rst_rate_norm = normalize_linear(snapshot.rst_rate, _params.max_rst_rate);
        
        // 6. 新连接速率（线性归一化）
        features.conn_established_norm = normalize_linear(
            snapshot.new_conn_rate, _params.max_conn_rate);
        
        // 7. SYN/SYN-ACK 比率（sigmoid 归一化）
        //    正常情况下约为 1.0，SYN Flood 时会很高
        //    使用 sigmoid 使高值不会过度主导特征
        features.tx_rx_ratio_norm = normalize_sigmoid(
            snapshot.syn_to_synack_ratio, _params.syn_ratio_warning);
        
        // 8. 活跃连接数（线性归一化）
        features.active_conn_norm = normalize_linear(
            static_cast<double>(active_connections), _params.max_active_conn);
        
        return features;
    }

    /**
     * 从 SecurityMetrics 快照直接提取 ISecurityModel::Input
     *
     * 消除中间转换，字段语义精确匹配安全模型的 8 维输入。
     * 用于 FirewallAI::run_inference()。
     *
     * @param snapshot 安全指标快照
     * @return ISecurityModel::Input 格式的特征向量
     */
    ISecurityModel::Input extract_security(const SecurityMetrics::Snapshot& snapshot) const {
        // 衍生指标
        double avg_pkt_size = (snapshot.pps > 0) ? (snapshot.bps / snapshot.pps) : 0.0;
        double rst_ratio = (snapshot.pps > 0) ? (snapshot.rst_rate / snapshot.pps) : 0.0;
        // SYN fraction: syn_rate / pps — volume-invariant attack signal
        // 比 syn_to_synack_ratio 更鲁棒（后者依赖 SYN-ACK 计数，在用户态协议栈中不可靠）
        double syn_fraction = (snapshot.pps > 0) ? (snapshot.syn_rate / snapshot.pps) : 0.0;

        return ISecurityModel::Input{
            .pps_norm           = normalize_linear(snapshot.pps, _params.max_pps),
            .bps_norm           = normalize_log(snapshot.bps, _params.max_bps),
            .syn_rate_norm      = normalize_linear(snapshot.syn_rate, _params.max_syn_rate),
            .rst_rate_norm      = normalize_linear(snapshot.rst_rate, _params.max_rst_rate),
            .syn_ratio_norm     = static_cast<float>(std::clamp(syn_fraction, 0.0, 1.0)),
            .new_conn_rate_norm = normalize_linear(snapshot.new_conn_rate, _params.max_conn_rate),
            .avg_pkt_size_norm  = normalize_linear(avg_pkt_size, 1500.0),
            .rst_ratio_norm     = static_cast<float>(std::clamp(rst_ratio, 0.0, 1.0)),
        };
    }

    /**
     * 从原始计数器提取特征（简化版，用于单个包级别的检测）
     *
     * @param pps 当前包速率
     * @param syn_rate 当前 SYN 速率
     * @param rst_rate 当前 RST 速率
     * @param avg_pkt_size 平均包大小
     * @return AnomalyInput 格式的特征向量
     */
    AnomalyInput extract_simple(double pps, double syn_rate, 
                                         double rst_rate, double avg_pkt_size) const {
        AnomalyInput features{};
        
        features.packets_rx_norm = normalize_linear(pps, _params.max_pps);
        features.packets_tx_norm = features.packets_rx_norm;
        features.bytes_tx_norm = normalize_linear(pps * avg_pkt_size, _params.max_bps);
        features.syn_rate_norm = normalize_linear(syn_rate, _params.max_syn_rate);
        features.rst_rate_norm = normalize_linear(rst_rate, _params.max_rst_rate);
        features.conn_established_norm = normalize_linear(syn_rate, _params.max_conn_rate);
        features.tx_rx_ratio_norm = 0.5f;  // 默认正常值
        features.active_conn_norm = 0.5f;  // 默认正常值
        
        return features;
    }

    // ─── 归一化参数访问 ───
    
    const NormalizationParams& params() const { return _params; }
    NormalizationParams& params() { return _params; }

private:
    NormalizationParams _params;

    // ─── 归一化方法 ───

    /**
     * 线性归一化: value / max, 裁剪到 [0, 1]
     */
    static float normalize_linear(double value, double max_value) {
        if (max_value <= 0) return 0.0f;
        return static_cast<float>(std::clamp(value / max_value, 0.0, 1.0));
    }

    /**
     * 对数归一化: log(1 + value) / log(1 + max), 裁剪到 [0, 1]
     * 适用于范围很大的值（如字节数）
     */
    static float normalize_log(double value, double max_value) {
        if (max_value <= 0 || value <= 0) return 0.0f;
        double log_value = std::log1p(value);
        double log_max = std::log1p(max_value);
        return static_cast<float>(std::clamp(log_value / log_max, 0.0, 1.0));
    }

    /**
     * Sigmoid 归一化: 1 / (1 + exp(-k * (value - midpoint)))
     * 适用于需要软饱和的值（如比率）
     *
     * @param value 输入值
     * @param midpoint 中点（sigmoid 输出为 0.5 的位置）
     * @param k 陡峭程度（默认 1.0）
     */
    static float normalize_sigmoid(double value, double midpoint, double k = 1.0) {
        double x = k * (value - midpoint);
        return static_cast<float>(1.0 / (1.0 + std::exp(-x)));
    }
};

/**
 * 安全特征向量 - 用于日志和调试
 *
 * 与 AnomalyInput 相同，但提供友好的打印方法
 */
struct SecurityFeatures {
    float packets_rx_norm;
    float packets_tx_norm;
    float bytes_tx_norm;
    float syn_rate_norm;
    float rst_rate_norm;
    float conn_established_norm;
    float tx_rx_ratio_norm;
    float active_conn_norm;
    
    // 从 AnomalyInput 构造
    static SecurityFeatures from(const AnomalyInput& input) {
        return {
            .packets_rx_norm = input.packets_rx_norm,
            .packets_tx_norm = input.packets_tx_norm,
            .bytes_tx_norm = input.bytes_tx_norm,
            .syn_rate_norm = input.syn_rate_norm,
            .rst_rate_norm = input.rst_rate_norm,
            .conn_established_norm = input.conn_established_norm,
            .tx_rx_ratio_norm = input.tx_rx_ratio_norm,
            .active_conn_norm = input.active_conn_norm,
        };
    }
    
    // 转换为 AnomalyInput
    AnomalyInput to_input() const {
        return {
            .packets_rx_norm = packets_rx_norm,
            .packets_tx_norm = packets_tx_norm,
            .bytes_tx_norm = bytes_tx_norm,
            .syn_rate_norm = syn_rate_norm,
            .rst_rate_norm = rst_rate_norm,
            .conn_established_norm = conn_established_norm,
            .tx_rx_ratio_norm = tx_rx_ratio_norm,
            .active_conn_norm = active_conn_norm,
        };
    }
};

} // namespace neustack

#endif // NEUSTACK_METRICS_SECURITY_FEATURES_HPP
