#ifndef NEUSTACK_AI_INTELLIGENCE_PLANE_HPP
#define NEUSTACK_AI_INTELLIGENCE_PLANE_HPP

#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/ai_action.hpp"
#include "neustack/metrics/ai_features.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/orca_model.hpp"
#include "neustack/ai/anomaly_model.hpp"
#include "neustack/ai/bandwidth_model.hpp"
#endif

#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

namespace neustack {

/**
 * 智能面配置
 */
struct IntelligencePlaneConfig {
    // 模型路径 (空字符串 = 禁用该模型)
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;

    // 推理间隔
    std::chrono::milliseconds orca_interval{10};       // 10ms
    std::chrono::milliseconds anomaly_interval{1000};  // 1s
    std::chrono::milliseconds bandwidth_interval{100}; // 100ms

    // 异常检测阈值
    float anomaly_threshold = 0.5f;

    // 带宽预测历史长度
    size_t bandwidth_history_length = 30;
};

/**
 * 智能面 - AI 推理线程
 *
 * 从数据面读取指标，运行 AI 模型，将决策发送回数据面
 */
class IntelligencePlane {
public:
    using Config = IntelligencePlaneConfig;

    IntelligencePlane(
        MetricsBuffer<TCPSample, 1024> &metrics_buf, // 数据面 → 读
        SPSCQueue<AIAction, 16> &action_queue,       // 写 → 数据面
        const Config& config = {}
    );

    ~IntelligencePlane();

    // 禁止拷贝和移动
    IntelligencePlane(const IntelligencePlane &) = delete;
    IntelligencePlane &operator=(const IntelligencePlane &) = delete;

    // 启动智能面线程
    void start();

    // 停止智能面线程
    void stop();

    // 是否正在运行
    bool is_running() const { return _running.load(std::memory_order_relaxed); }

private:
    // 数据通道
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    SPSCQueue<AIAction, 16>& _action_queue;

    // AI 模型
#ifdef NEUSTACK_AI_ENABLED
    std::unique_ptr<OrcaModel> _orca_model;
    std::unique_ptr<AnomalyDetector> _anomaly_model;
    std::unique_ptr<BandwidthPredictor> _bandwidth_model;
#endif

    // 状态
    Config _config;
    std::atomic<bool> _running;
    std::thread _thread;

    // 历史数据 (用于带宽预测)
    std::vector<TCPSample> _sample_history;
    GlobalMetrics::Snapshot _prev_snapshot;
    GlobalMetrics::Snapshot _anomaly_prev_snapshot;  // anomaly 独立快照，避免 1ms delta 过小

    // 带宽预测结果缓存 (raw bytes/s, 供 Orca 归一化使用)
    float _cached_predicted_bw = 0.0f;

    // 估计带宽 (sliding window max delivery_rate, bytes/s)
    uint32_t _est_bw = 0;

    // 内部方法
    void run_loop();

    void process_orca();
    void process_anomaly(const GlobalMetrics::Snapshot::Delta& delta, uint32_t active_connections);
    void process_bandwidth();

    // 时间戳
    std::chrono::steady_clock::time_point _last_orca_time;
    std::chrono::steady_clock::time_point _last_anomaly_time;
    std::chrono::steady_clock::time_point _last_bandwidth_time;
};

} // namespace neustack


#endif // NEUSTACK_AI_INTELLIGENCE_PLANE_HPP