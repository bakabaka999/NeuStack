#ifndef NEUSTACK_FIREWALL_FIREWALL_AI_HPP
#define NEUSTACK_FIREWALL_FIREWALL_AI_HPP

#include "neustack/metrics/security_metrics.hpp"
#include "neustack/metrics/security_features.hpp"
#include "neustack/firewall/firewall_decision.hpp"
#include "neustack/firewall/packet_event.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/security_model.hpp"
#endif

#include <memory>
#include <atomic>
#include <chrono>
#include <functional>

namespace neustack {

/**
 * 防火墙 AI 配置
 */
struct FirewallAIConfig {
    // ─── AI 模型配置 ───
    std::string model_path;           // ONNX 模型路径（空 = 禁用 AI）
    float anomaly_threshold = 0.5f;   // 异常阈值（重构误差）
    
    // ─── Shadow Mode 配置 ───
    bool shadow_mode = true;          // Shadow Mode: AI 判定异常时只告警不阻断
    
    // ─── 推理间隔配置 ───
    // AI 推理相对较慢，不应每个包都调用
    // 改为定期推理，缓存结果
    uint32_t inference_interval_ms = 1000;  // 推理间隔（毫秒）
    
    // ─── 告警回调 ───
    // 当 AI 检测到异常时调用（无论 Shadow Mode 是否开启）
    // 可用于日志、通知、指标上报等
    using AlertCallback = std::function<void(float anomaly_score, 
                                              const SecurityMetrics::Snapshot& snapshot)>;
};

/**
 * 防火墙 AI 统计
 */
struct FirewallAIStats {
    uint64_t inferences_total = 0;    // 总推理次数
    uint64_t anomalies_detected = 0;  // 检测到的异常次数
    uint64_t alerts_triggered = 0;    // 触发告警次数
    uint64_t drops_by_ai = 0;         // AI 导致的丢包（非 Shadow Mode）
    
    float last_anomaly_score = 0.0f;  // 最近一次异常分数
    float max_anomaly_score = 0.0f;   // 历史最高异常分数
    
    // 最近一次异常的时间戳（毫秒）
    uint64_t last_anomaly_time_ms = 0;
};

/**
 * 防火墙 AI 层 - 集成 AI 异常检测到防火墙
 *
 * 设计原则:
 * - 不阻塞热路径：AI 推理定期执行，结果缓存
 * - Shadow Mode：默认只告警不阻断，便于调试和观察
 * - 可配置：阈值、推理间隔、告警回调都可自定义
 *
 * 使用模式:
 * 1. 数据面每个包调用 record_packet() 更新指标
 * 2. 定时器每秒调用 tick() 更新滑动窗口
 * 3. 定时器每 N 秒调用 run_inference() 执行 AI 推理
 * 4. 数据面调用 evaluate() 获取 AI 决策（使用缓存的结果）
 *
 * 线程安全:
 * - record_packet() 可从任意线程调用
 * - tick() 和 run_inference() 应从单一线程调用（定时器线程）
 * - evaluate() 可从任意线程调用（读取原子缓存）
 */
class FirewallAI {
public:
    using Config = FirewallAIConfig;
    using Stats = FirewallAIStats;

    // ─── 构造与配置 ───

    explicit FirewallAI(const Config& config = {});
    ~FirewallAI() = default;

    // 禁止拷贝
    FirewallAI(const FirewallAI&) = delete;
    FirewallAI& operator=(const FirewallAI&) = delete;

    /**
     * 加载 AI 模型
     *
     * @param model_path ONNX 模型路径
     * @return true 如果加载成功
     */
    bool load_model(const std::string& model_path);

    /**
     * AI 模型是否已加载
     */
    bool is_loaded() const;

    // ─── 数据面调用 ───

    /**
     * 记录数据包（更新安全指标）
     *
     * @param evt 解析后的包事件
     */
    void record_packet(const PacketEvent& evt);

    /**
     * 评估当前流量状态
     *
     * 使用缓存的 AI 推理结果，不会阻塞。
     *
     * @return FirewallDecision AI 的决策
     *         - PASS: 正常流量
     *         - ALERT: 异常流量（Shadow Mode）
     *         - DROP: 异常流量（非 Shadow Mode）
     */
    FirewallDecision evaluate() const;

    // ─── 定时器调用 ───

    /**
     * 滑动窗口前进一格
     *
     * 应该每秒调用一次。
     */
    void tick();

    /**
     * 执行 AI 推理
     *
     * 应该每 inference_interval_ms 调用一次。
     * 推理结果会缓存，供 evaluate() 使用。
     *
     * @return 异常分数（0.0 = 正常，越高越异常）
     */
    float run_inference();

    // ─── 配置 API ───

    void set_threshold(float threshold);
    float threshold() const { return _config.anomaly_threshold; }

    void set_shadow_mode(bool shadow) { _config.shadow_mode = shadow; }
    bool shadow_mode() const { return _config.shadow_mode; }

    uint32_t inference_interval_ms() const { return _config.inference_interval_ms; }
    void set_inference_interval_ms(uint32_t ms) { _config.inference_interval_ms = ms; }

    void set_alert_callback(Config::AlertCallback cb) { _alert_callback = std::move(cb); }

    // ─── 统计 API ───

    const Stats& stats() const { return _stats; }
    void reset_stats() { _stats = {}; }

    // ─── 指标访问 ───

    const SecurityMetrics& metrics() const { return _metrics; }
    SecurityMetrics::Snapshot metrics_snapshot() const { return _metrics.snapshot(); }

    // ─── 特征提取器访问 ───

    SecurityFeatureExtractor& feature_extractor() { return _extractor; }
    const SecurityFeatureExtractor& feature_extractor() const { return _extractor; }

private:
    // ─── 配置 ───
    Config _config;

    // ─── AI 模型 ───
#ifdef NEUSTACK_AI_ENABLED
    std::unique_ptr<SecurityAnomalyModel> _model;
#endif

    // ─── 安全指标 ───
    SecurityMetrics _metrics;

    // ─── 特征提取器 ───
    SecurityFeatureExtractor _extractor;

    // ─── 缓存的推理结果 ───
    std::atomic<float> _cached_anomaly_score{0.0f};
    std::atomic<bool> _cached_is_anomaly{false};

    // ─── 时间戳 ───
    std::chrono::steady_clock::time_point _last_inference_time;

    // ─── 统计 ───
    Stats _stats;

    // ─── 回调 ───
    Config::AlertCallback _alert_callback;

    // ─── 内部方法 ───

    // 获取当前时间（毫秒）
    static uint64_t now_ms();
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_FIREWALL_AI_HPP
