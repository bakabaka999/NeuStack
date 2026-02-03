#include "neustack/ai/intelligence_plane.hpp"
#include "neustack/common/log.hpp"

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
            }

            // 限制历史长度
            constexpr size_t MAX_HISTORY = 1000;
            if (_sample_history.size() > MAX_HISTORY) {
                _sample_history.erase(
                    _sample_history.begin(),
                    _sample_history.begin() + (_sample_history.size() - MAX_HISTORY)
                );
            }

            last_read_count = current_count;
        }

        // ─── 2. 读取全局统计快照 ───
        auto snapshot = global_metrics().snapshot();
        auto delta = snapshot.diff(_prev_snapshot);
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
            process_anomaly(delta);
            _last_anomaly_time = now;
        }

        // 带宽预测（中频）
        if (_bandwidth_model && _bandwidth_model->is_loaded() &&
            now - _last_bandwidth_time >= _config.bandwidth_interval) {
            process_bandwidth();
            _last_bandwidth_time = now;
        }
#else
        (void)delta;  // 避免未使用警告
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

    // 构造 Orca 输入特征
    // 需要归一化参数（这里使用简单的经验值）
    constexpr float MAX_THROUGHPUT = 100e6f;  // 100 Mbps
    constexpr float MAX_DELAY = 100000.0f;    // 100ms in us
    constexpr float MAX_CWND = 1000.0f;       // 1000 MSS

    ICongestionModel::Input input{
        .throughput_normalized = static_cast<float>(sample.delivery_rate) / MAX_THROUGHPUT,
        .queuing_delay_normalized = static_cast<float>(sample.queuing_delay_us()) / MAX_DELAY,
        .rtt_ratio = sample.rtt_ratio(),
        .loss_rate = sample.loss_rate(),
        .cwnd_normalized = static_cast<float>(sample.cwnd) / MAX_CWND,
        .in_flight_ratio = (sample.cwnd > 0)
            ? static_cast<float>(sample.bytes_in_flight) / (sample.cwnd * 1460)  // 假设 MSS=1460
            : 0.0f,
        .predicted_bw_normalized = _cached_predicted_bw
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
void IntelligencePlane::process_anomaly(const GlobalMetrics::Snapshot::Delta& delta) {
#ifdef NEUSTACK_AI_ENABLED
    // 计算速率（假设 1 秒间隔）
    float interval_sec = static_cast<float>(_config.anomaly_interval.count()) / 1000.0f;
    if (interval_sec <= 0) interval_sec = 1.0f;

    // 归一化参数
    constexpr float MAX_RATE = 10000.0f;  // 10k/s
    constexpr float MAX_PACKET_SIZE = 1500.0f;

    float avg_packet_size = (delta.packets_rx > 0)
        ? static_cast<float>(delta.bytes_rx) / delta.packets_rx
        : 0.0f;

    IAnomalyModel::Input input{
        .syn_rate = static_cast<float>(delta.syn_received) / interval_sec / MAX_RATE,
        .rst_rate = static_cast<float>(delta.rst_received) / interval_sec / MAX_RATE,
        .new_conn_rate = static_cast<float>(delta.conn_established) / interval_sec / MAX_RATE,
        .packet_rate = static_cast<float>(delta.packets_rx) / interval_sec / MAX_RATE,
        .avg_packet_size = avg_packet_size / MAX_PACKET_SIZE
    };

    auto result = _anomaly_model->infer(input);
    if (result && result->is_anomaly) {
        AIAction action{};
        action.type = AIAction::Type::ANOMALY_ALERT;
        action.conn_id = 0;  // 全局
        action.anomaly.score = result->reconstruction_error;

        _action_queue.try_push(action);
        LOG_WARN(AI, "Anomaly detected! score=%.4f", result->reconstruction_error);
    }
#else
    (void)delta;
#endif
}

// ─── 带宽预测推理 ───
void IntelligencePlane::process_bandwidth() {
#ifdef NEUSTACK_AI_ENABLED
    size_t required = _bandwidth_model->required_history_length();
    if (_sample_history.size() < required) {
        return;  // 历史数据不足
    }

    // 构造输入：提取最近 N 个样本的 throughput/rtt/loss
    IBandwidthModel::Input input;
    input.throughput_history.reserve(required);
    input.rtt_history.reserve(required);
    input.loss_history.reserve(required);

    // 归一化参数
    constexpr float MAX_THROUGHPUT = 100e6f;  // 100 Mbps
    constexpr float MAX_RTT = 100000.0f;      // 100ms in us

    size_t start = _sample_history.size() - required;
    for (size_t i = 0; i < required; i++) {
        const auto& sample = _sample_history[start + i];
        input.throughput_history.push_back(
            static_cast<float>(sample.delivery_rate) / MAX_THROUGHPUT
        );
        input.rtt_history.push_back(
            static_cast<float>(sample.rtt_us) / MAX_RTT
        );
        input.loss_history.push_back(sample.loss_rate());
    }

    auto result = _bandwidth_model->infer(input);
    if (result) {
        // 缓存预测结果 (归一化后供 Orca 使用)
        _cached_predicted_bw = static_cast<float>(result->predicted_bandwidth) / MAX_THROUGHPUT;

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