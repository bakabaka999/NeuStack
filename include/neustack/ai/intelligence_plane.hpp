#ifndef NEUSTACK_AI_INTELLIGENCE_PLANE_HPP
#define NEUSTACK_AI_INTELLIGENCE_PLANE_HPP

#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/ai_action.hpp"
#include <thread>
#include <atomic>

namespace neustack {
    
class IntelligencePlane {
public:
    IntelligencePlane(
        MetricsBuffer<TCPSample, 1024>& metrics_buf,    // 数据面 → 读
        SPSCQueue<AIAction, 16>& action_queue           // 写 → 数据面
    )
        : _metrics_buf(metrics_buf)
        , _action_queue(action_queue)
        , _running(false)
    {}

    void start() {
        _running = true;
        _thread = std::thread([this] {run();});
    }

    void stop() {
        _running = false;
        if (_thread.joinable()) _thread.join();
    }

private:
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    SPSCQueue<AIAction, 16>& _action_queue;
    std::atomic<bool> _running;
    std::thread _thread;

    void run() {
        GlobalMetrics::Snapshot prev_snapshot = {};

        while (_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // ─── 1. 读取采样数据 ───
            auto samples = _metrics_buf.recent(100);

            // ─── 2. 读取全局统计快照 ───
            auto snapshot = global_metrics().snapshot();
            auto delta = snapshot.diff(prev_snapshot);
            prev_snapshot = snapshot;

            // ─── 3. AI 推理 (后续实现) ───
            // auto features = extract_features(samples);
            // auto action = onnx_infer(features);

            // ─── 4. 传回数据面 ───
            // _action_queue.try_push(action);
        }
    }
};

} // namespace neustack


#endif // NEUSTACK_AI_INTELLIGENCE_PLANE_HPP