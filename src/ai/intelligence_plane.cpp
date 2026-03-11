#include "neustack/ai/intelligence_plane.hpp"
#include "neustack/common/log.hpp"
#include <algorithm>

using namespace neustack;

IntelligencePlane::IntelligencePlane(
    MetricsBuffer<TCPSample, 1024>& metrics_buf,
    SPSCQueue<AIAction, 16>& action_queue,
    const Config& config
)
    : _metrics_buf(metrics_buf)
    , _action_queue(action_queue)
    , _config(config)
{
#ifdef NEUSTACK_AI_ENABLED
    // 加载模型
    if (!config.orca_model_path.empty()) {
        _orca_model = std::make_unique<OrcaModel>(config.orca_model_path);
    }

    if (!config.anomaly_model_path.empty()) {
        _anomaly_model = std::make_unique<AnomalyDetector>(
            config.anomaly_model_path,
            config.anomaly_threshold
        );
    }

    if (!config.bandwidth_model_path.empty()) {
        _bandwidth_model = std::make_unique<BandwidthPredictor>(
            config.bandwidth_model_path,
            config.bandwidth_history_length
        );
    }
#endif

    // 初始化快照
    _prev_snapshot = global_metrics().snapshot();
    _anomaly_prev_snapshot = _prev_snapshot;
}

IntelligencePlane::~IntelligencePlane() {
    stop();
}

void IntelligencePlane::start() {
    // 利用XCHG原子化检查+设置
    if (_running.exchange(true)) {
        return; // 已经在运行了
    }

    _thread = std::thread(&IntelligencePlane::run_loop, this);
    LOG_INFO(AI, "Intelligence plane started");
}

void IntelligencePlane::stop() {
    if (!_running.exchange(false)) {
        return;  // 已经停止
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    LOG_INFO(AI, "Intelligence plane stopped");
}

void IntelligencePlane::run_loop() {
    auto now = std::chrono::steady_clock::now();
    _last_orca_time = now;
    _last_anomaly_time = now;
    _last_bandwidth_time = now;

    size_t last_read_count = 0;  // 上次读取时的 total_pushed

    while (_running.load(std::memory_order_relaxed)) {
        now = std::chrono::steady_clock::now();

        // ─── 1. 读取新的 TCPSample ───
        size_t current_count = _metrics_buf.total_pushed();
        if (current_count > last_read_count) {
            // 有新数据，读取增量
            size_t new_samples = current_count - last_read_count;
            auto samples = _metrics_buf.recent(std::min(new_samples, size_t(100)));

            // 追加到历史（用于带宽预测）
            for (const auto& sample : samples) {
                _sample_history.push_back(sample);

                // 更新 est_bw (sliding window max delivery_rate)
                if (sample.delivery_rate > _est_bw) {
                    _est_bw = sample.delivery_rate;
                }
            }

            // 限制历史长度
            constexpr size_t MAX_HISTORY = 1000;
            if (_sample_history.size() > MAX_HISTORY) {
                _sample_history.erase(
                    _sample_history.begin(),
                    _sample_history.begin() + (_sample_history.size() - MAX_HISTORY)
                );
                // Recompute est_bw from remaining history
                _est_bw = 0;
                for (const auto& s : _sample_history) {
                    if (s.delivery_rate > _est_bw) {
                        _est_bw = s.delivery_rate;
                    }
                }
            }

            last_read_count = current_count;
        }

        // ─── 2. 读取全局统计快照 ───
        auto snapshot = global_metrics().snapshot();
        _prev_snapshot = snapshot;

#ifdef NEUSTACK_AI_ENABLED
        // ─── 3. 按间隔执行各模型推理 ───

        // Orca 拥塞控制（高频）
        if (_orca_model && _orca_model->is_loaded() &&
            now - _last_orca_time >= _config.orca_interval) {
            process_orca();
            _last_orca_time = now;
        }

        // 异常检测（低频）
        if (_anomaly_model && _anomaly_model->is_loaded() &&
            now - _last_anomaly_time >= _config.anomaly_interval) {
            // 用独立快照计算完整 1s 的 delta，而非主循环的 1ms delta
            auto anomaly_delta = snapshot.diff(_anomaly_prev_snapshot);
            _anomaly_prev_snapshot = snapshot;
            process_anomaly(anomaly_delta, snapshot.active_connections);
            _last_anomaly_time = now;
        }

        // 带宽预测（中频）
        if (_bandwidth_model && _bandwidth_model->is_loaded() &&
            now - _last_bandwidth_time >= _config.bandwidth_interval) {
            process_bandwidth();
            _last_bandwidth_time = now;
        }
#else
        (void)snapshot;
#endif

        // 短暂休眠，避免空转
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ─── Orca 拥塞控制推理 ───
void IntelligencePlane::process_orca() {
#ifdef NEUSTACK_AI_ENABLED
    if (_sample_history.empty()) return;

    // 使用最新的样本
    const auto& sample = _sample_history.back();

    // 使用 OrcaFeatures 构造正确归一化的输入
    auto features = OrcaFeatures::from_sample(sample, _est_bw, _cached_predicted_bw);
    auto feature_vec = features.to_vector();

    ICongestionModel::Input input{
        .throughput_normalized = feature_vec[0],
        .queuing_delay_normalized = feature_vec[1],
        .rtt_ratio = feature_vec[2],
        .loss_rate = feature_vec[3],
        .cwnd_normalized = feature_vec[4],
        .in_flight_ratio = feature_vec[5],
        .predicted_bw_normalized = feature_vec[6]
    };

    auto result = _orca_model->infer(input);
    if (result) {
        AIAction action{};
        action.type = AIAction::Type::CWND_ADJUST;
        action.conn_id = 0;  // 全局（暂时）
        action.cwnd.alpha = result->alpha;

        _action_queue.try_push(action);
        LOG_DEBUG(AI, "Orca: alpha=%.3f", result->alpha);
    }
#endif
}

// ─── 异常检测推理 ───
void IntelligencePlane::process_anomaly(
    const GlobalMetrics::Snapshot::Delta& delta,
    uint32_t active_connections
) {
#ifdef NEUSTACK_AI_ENABLED
    // 使用 AnomalyFeatures 构造正确归一化的 8-dim 输入
    auto features = AnomalyFeatures::from_delta(delta, active_connections);

    // 低负载时跳过推理:
    // - 极低流量时 ratio 特征失去统计意义 (1 SYN / 7 pkts = 14% 是噪声不是攻击)
    // - 至少需要 ~100 个包/秒 ratio 才有意义
    // log1p(100)/log1p(40000) ≈ 0.44
    constexpr float MIN_LOG_PKT_RATE = 0.44f;
    if (features.log_pkt_rate < MIN_LOG_PKT_RATE) {
        // 推送 score=0 让 UI 显示空闲状态
        AIAction action{};
        action.type = AIAction::Type::ANOMALY_ALERT;
        action.conn_id = 0;
        action.anomaly.score = 0.0f;
        _action_queue.try_push(action);
        return;
    }

    IAnomalyModel::Input input{
        .log_pkt_rate = features.log_pkt_rate,
        .bytes_per_pkt = features.bytes_per_pkt,
        .syn_ratio = features.syn_ratio,
        .rst_ratio = features.rst_ratio,
        .conn_completion = features.conn_completion,
        .tx_rx_ratio = features.tx_rx_ratio,
        .log_active_conn = features.log_active_conn,
        .log_conn_reset = features.log_conn_reset
    };

    LOG_DEBUG(AI, "Anomaly features: lpkt=%.3f bpp=%.3f syn=%.3f rst=%.3f "
             "comp=%.3f txrx=%.3f lconn=%.3f lreset=%.3f",
             features.log_pkt_rate, features.bytes_per_pkt,
             features.syn_ratio, features.rst_ratio,
             features.conn_completion, features.tx_rx_ratio,
             features.log_active_conn, features.log_conn_reset);

    auto result = _anomaly_model->infer(input);
    if (result) {
        // 始终推送 anomaly score，让 UI 显示实时重构误差
        AIAction action{};
        action.type = AIAction::Type::ANOMALY_ALERT;
        action.conn_id = 0;  // 全局
        action.anomaly.score = result->reconstruction_error;

        _action_queue.try_push(action);

        if (result->is_anomaly) {
            LOG_WARN(AI, "Anomaly detected! score=%.4f "
                     "[lpkt=%.3f bpp=%.3f syn=%.3f rst=%.3f comp=%.3f txrx=%.3f lconn=%.3f lreset=%.3f]",
                     result->reconstruction_error,
                     features.log_pkt_rate, features.bytes_per_pkt,
                     features.syn_ratio, features.rst_ratio,
                     features.conn_completion, features.tx_rx_ratio,
                     features.log_active_conn, features.log_conn_reset);
        }
    }
#else
    (void)delta;
    (void)active_connections;
#endif
}

// ─── 带宽预测推理 ───
void IntelligencePlane::process_bandwidth() {
#ifdef NEUSTACK_AI_ENABLED
    size_t required = _bandwidth_model->required_history_length();
    if (_sample_history.size() < required) {
        return;  // 历史数据不足
    }

    // 提取最近 N 个样本
    size_t start = _sample_history.size() - required;
    std::vector<TCPSample> recent_samples(
        _sample_history.begin() + start,
        _sample_history.end()
    );

    // 计算 min_rtt from recent samples
    uint32_t min_rtt_us = UINT32_MAX;
    for (const auto& s : recent_samples) {
        if (s.min_rtt_us > 0 && s.min_rtt_us < min_rtt_us) {
            min_rtt_us = s.min_rtt_us;
        }
    }
    if (min_rtt_us == UINT32_MAX) min_rtt_us = 0;

    // 使用 BandwidthFeatures 构造正确归一化的输入
    auto features = BandwidthFeatures::from_samples(recent_samples, min_rtt_us);

    IBandwidthModel::Input input;
    input.throughput_history = std::move(features.throughput_history);
    input.rtt_history = std::move(features.rtt_history);
    input.loss_history = std::move(features.loss_history);

    auto result = _bandwidth_model->infer(input);
    if (result) {
        // 缓存预测结果 (raw bytes/s, Orca 归一化时会除以 MAX_BW)
        _cached_predicted_bw = static_cast<float>(result->predicted_bandwidth);

        AIAction action{};
        action.type = AIAction::Type::BW_PREDICTION;
        action.conn_id = 0;  // 全局
        action.bandwidth.predicted_bw = result->predicted_bandwidth;

        _action_queue.try_push(action);
        LOG_DEBUG(AI, "Bandwidth prediction: %u bytes/s (conf=%.2f)",
                  result->predicted_bandwidth, result->confidence);
    }
#endif
}