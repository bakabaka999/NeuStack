#include "neustack/firewall/firewall_ai.hpp"
#include "neustack/common/log.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/security_model.hpp"
#endif

#include <chrono>
#include <algorithm>

namespace neustack {

// ============================================================================
// 构造函数
// ============================================================================

FirewallAI::FirewallAI(const Config& config)
    : _config(config)
    , _last_inference_time(std::chrono::steady_clock::now())
{
    // 如果配置了模型路径，尝试加载
    if (!config.model_path.empty()) {
        load_model(config.model_path);
    }
}

// ============================================================================
// 模型管理
// ============================================================================

bool FirewallAI::load_model(const std::string& model_path) {
#ifdef NEUSTACK_AI_ENABLED
    try {
        _model = std::make_unique<SecurityAnomalyModel>(model_path, _config.anomaly_threshold);
        
        if (_model->is_loaded()) {
            LOG_INFO(FW, "AI model loaded: %s (threshold=%.3f)", 
                     model_path.c_str(), _config.anomaly_threshold);
            return true;
        } else {
            LOG_ERROR(FW, "Failed to load AI model: %s", model_path.c_str());
            _model.reset();
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR(FW, "Exception loading AI model: %s", e.what());
        _model.reset();
        return false;
    }
#else
    // AI 未启用时，记录警告并返回 false
    (void)model_path;
    LOG_WARN(FW, "AI features not enabled at compile time (NEUSTACK_AI_ENABLED)");
    return false;
#endif
}

bool FirewallAI::is_loaded() const {
#ifdef NEUSTACK_AI_ENABLED
    return _model && _model->is_loaded();
#else
    return false;
#endif
}

// ============================================================================
// 数据面调用
// ============================================================================

void FirewallAI::record_packet(const PacketEvent& evt) {
    // 更新安全指标
    _metrics.record_packet(evt.total_len, evt.tcp_flags);
}

FirewallDecision FirewallAI::evaluate() const {
    // 如果 AI 未加载，默认放行
    if (!is_loaded()) {
        return FirewallDecision::pass();
    }

    // 读取缓存的推理结果
    bool is_anomaly = _cached_is_anomaly.load(std::memory_order_relaxed);
    float score = _cached_anomaly_score.load(std::memory_order_relaxed);

    if (!is_anomaly) {
        // 正常流量，放行
        return FirewallDecision::pass();
    }

    // 异常流量
    if (_config.shadow_mode) {
        // Shadow Mode: 只告警不阻断
        return FirewallDecision{
            .action = FirewallDecision::Action::ALERT,
            .reason = FirewallDecision::Reason::AI_ANOMALY,
            .rule_id = 0,
            .score = score,
            .rate_current = 0,
            .rate_limit = 0
        };
    } else {
        // 非 Shadow Mode: 直接丢弃
        return FirewallDecision{
            .action = FirewallDecision::Action::DROP,
            .reason = FirewallDecision::Reason::AI_ANOMALY,
            .rule_id = 0,
            .score = score,
            .rate_current = 0,
            .rate_limit = 0
        };
    }
}

// ============================================================================
// 定时器调用
// ============================================================================

void FirewallAI::tick() {
    _metrics.tick();
}

float FirewallAI::run_inference() {
#ifdef NEUSTACK_AI_ENABLED
    if (!_model || !_model->is_loaded()) {
        return 0.0f;
    }

    // 获取当前指标快照
    auto snapshot = _metrics.snapshot();

    // 更新推理时间戳
    _last_inference_time = std::chrono::steady_clock::now();

    // 直接提取 ISecurityModel::Input（无中间转换）
    auto model_input = _extractor.extract_security(snapshot);

    // 执行推理
    auto result = _model->infer(model_input);

    if (!result.has_value()) {
        LOG_WARN(FW, "AI inference failed");
        return 0.0f;
    }

    float score = result->reconstruction_error;
    bool is_anomaly = result->is_anomaly;

    // 更新缓存
    _cached_anomaly_score.store(score, std::memory_order_relaxed);
    _cached_is_anomaly.store(is_anomaly, std::memory_order_relaxed);

    // 更新统计
    _stats.inferences_total++;
    _stats.last_anomaly_score = score;
    if (score > _stats.max_anomaly_score) {
        _stats.max_anomaly_score = score;
    }

    if (is_anomaly) {
        _stats.anomalies_detected++;
        _stats.last_anomaly_time_ms = now_ms();
        _stats.alerts_triggered++;

        // 调用告警回调
        if (_alert_callback) {
            _alert_callback(score, snapshot);
        }

        // 记录日志
        LOG_WARN(FW, "[AI ANOMALY] score=%.4f threshold=%.4f syn_rate=%.1f rst_rate=%.1f pps=%.1f",
                 score, _config.anomaly_threshold,
                 snapshot.syn_rate, snapshot.rst_rate, snapshot.pps);

        // 更新指标
        if (_config.shadow_mode) {
            _metrics.record_alert();
        } else {
            _stats.drops_by_ai++;
        }
    }

    return score;
#else
    return 0.0f;
#endif
}

// ============================================================================
// 配置 API
// ============================================================================

void FirewallAI::set_threshold(float threshold) {
    _config.anomaly_threshold = threshold;
#ifdef NEUSTACK_AI_ENABLED
    if (_model) {
        _model->set_threshold(threshold);
    }
#endif
}

// ============================================================================
// 辅助方法
// ============================================================================

uint64_t FirewallAI::now_ms() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(ms.count());
}

} // namespace neustack
