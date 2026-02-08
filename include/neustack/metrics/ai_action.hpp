#ifndef NEUSTACK_METRICS_AI_ACTION_HPP
#define NEUSTACK_METRICS_AI_ACTION_HPP

#include <cstdint>

namespace neustack {

/**
 * 智能面 → 数据面的 AI 决策
 *
 * 通过 SPSCQueue 传递，必须是 trivially copyable
 */
struct AIAction {
    enum class Type : uint8_t {
        NONE = 0,       
        CWND_ADJUST,    // Orca：调整拥塞窗口
        ANOMALY_ALERT,  // 异常检测：发现异常
        BW_PREDICTION,  // 带宽预测：更新预测值
    };

    Type type = Type::NONE;
    uint32_t conn_id = 0;   // 目标连接（0 = 全局）

    struct Cwnd {
        float alpha;          // cwnd_new = 2^alpha * cwnd_cubic
    };
    struct Anomaly {
        float score;          // 异常分数 [0, 1]
    };
    struct Bw {
        uint32_t predicted_bw; // 预测带宽 (bytes/s)
    };

    union {
        Cwnd cwnd;
        Anomaly anomaly;
        Bw bandwidth;
    };
};

static_assert(sizeof(AIAction) <= 16, "AIAction should be compact");

} // namespace neustack 


#endif // NEUSTACK_METRICS_AI_ACTION_HPP