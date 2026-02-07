#ifndef NEUSTACK_AI_AI_AGENT_HPP
#define NEUSTACK_AI_AI_AGENT_HPP

#include <cstdint>
#include <chrono>
#include <deque>
#include <string>

namespace neustack {

// 策略状态机
enum class AgentState {
    NORMAL,
    DEGRADED,
    UNDER_ATTACK,
    RECOVERY
};

// 状态名称（日志/API 用）
inline const char* agent_state_name(AgentState s) {
    switch (s) {
        case AgentState::NORMAL:       return "NORMAL";
        case AgentState::DEGRADED:     return "DEGRADED";
        case AgentState::UNDER_ATTACK: return "UNDER_ATTACK";
        case AgentState::RECOVERY:     return "RECOVERY";
    }
    return "UNKNOWN";
}

// 决策日志条目
struct Decision {
    uint64_t timestamp_us;
    AgentState from_state, to_state;
    std::string reason;

    Decision(uint64_t ts, AgentState f, AgentState t, std::string r)
        : timestamp_us(ts), from_state(f), to_state(t), reason(std::move(r)) {}
};

// 决策层类
class NetworkAgent {
public:
    static constexpr size_t MAX_HISTORY_SIZE = 1000;
    static constexpr float DEFAULT_ANOMALY_THRESHOLD = 0.01f;
    static constexpr int ANOMALY_CLEAR_REQUIRED = 50;    // 连续 50 次正常才恢复
    static constexpr int RECOVERY_TICKS_REQUIRED = 100;  // RECOVERY 稳定 100 个 tick 后回 NORMAL
    static constexpr float BW_DROP_RATIO = 0.5f;         // 带宽骤降 50% 触发 DEGRADED
    static constexpr float BW_RECOVER_RATIO = 0.85f;     // 带宽恢复到骤降前 85% 解除 DEGRADED

    explicit NetworkAgent(float anomaly_threshold = DEFAULT_ANOMALY_THRESHOLD)
        : _anomaly_threshold(anomaly_threshold) {}

    // 状态转移输入
    void on_cwnd_adjust(float alpha);   // 收到 Orca 输出
    void on_anomaly(float score);       // 收到异常检测结果
    void on_bw_prediction(uint32_t bw); // 收到带宽预测

    // 对外接口
    AgentState state() const { return _state; }
    float effective_alpha() const;
    bool should_accept_connection() const;

    // 查询当前缓存值（API 用）
    float anomaly_score() const { return _anomaly_score; }
    uint32_t predicted_bw() const { return _predicted_bw; }
    float current_alpha() const { return _current_alpha; }

    // 日志接口
    const std::deque<Decision>& history() const { return _history; }

private:
    // 内部方法
    void transition_to(AgentState new_state, const std::string& reason);
    uint64_t get_now_us() const;

    // 状态
    AgentState _state = AgentState::NORMAL;
    float _anomaly_threshold;

    // 缓存的模型输出
    float _anomaly_score = 0.0f;
    uint32_t _predicted_bw = 0;
    float _current_alpha = 0.0f;

    // 异常检测相关
    int _anomaly_clear_count = 0;

    // RECOVERY 倒计时
    int _recovery_ticks = 0;

    // 带宽预测相关
    float _prev_predicted_bw = 0.0f;
    float _pre_degraded_bw = 0.0f;  // 进入 DEGRADED 前的带宽（用于判断恢复）

    // 决策日志
    std::deque<Decision> _history;
};

} // namespace neustack

#endif // NEUSTACK_AI_AI_AGENT_HPP