#include "neustack/ai/ai_agent.hpp"
#include <algorithm>

namespace neustack {

// ============================================================================
// on_cwnd_adjust - 收到 Orca alpha
// ============================================================================
void NetworkAgent::on_cwnd_adjust(float alpha) {
    _current_alpha = alpha;

    // RECOVERY 状态下，每收到一次 alpha 就计一个 tick
    // 这是最频繁的调用（10ms 一次），适合当计时器
    if (_state == AgentState::RECOVERY) {
        _recovery_ticks++;
        if (_recovery_ticks >= RECOVERY_TICKS_REQUIRED) {
            transition_to(AgentState::NORMAL, "recovery period completed");
        }
    }
}

// ============================================================================
// on_anomaly - 收到异常检测结果
// ============================================================================
void NetworkAgent::on_anomaly(float score) {
    _anomaly_score = score;

    if (score > _anomaly_threshold) {
        // 异常！重置消退计数
        _anomaly_clear_count = 0;

        // 从任何非 UNDER_ATTACK 状态进入攻击状态
        if (_state != AgentState::UNDER_ATTACK) {
            transition_to(AgentState::UNDER_ATTACK,
                "anomaly_score=" + std::to_string(score) +
                " > threshold=" + std::to_string(_anomaly_threshold));
        }
    } else {
        // 正常
        if (_state == AgentState::UNDER_ATTACK) {
            _anomaly_clear_count++;
            if (_anomaly_clear_count >= ANOMALY_CLEAR_REQUIRED) {
                transition_to(AgentState::RECOVERY,
                    "anomaly clear for " + std::to_string(_anomaly_clear_count) + " consecutive periods");
            }
        }
    }
}

// ============================================================================
// on_bw_prediction - 收到带宽预测
// ============================================================================
void NetworkAgent::on_bw_prediction(uint32_t bw) {
    _prev_predicted_bw = static_cast<float>(_predicted_bw);
    _predicted_bw = bw;

    if (_state == AgentState::NORMAL && _prev_predicted_bw > 0) {
        // 检测带宽骤降
        float drop_ratio = 1.0f - static_cast<float>(bw) / _prev_predicted_bw;
        if (drop_ratio > BW_DROP_RATIO) {
            _pre_degraded_bw = _prev_predicted_bw;  // 记住骤降前的水平
            transition_to(AgentState::DEGRADED,
                "bandwidth dropped " + std::to_string(static_cast<int>(drop_ratio * 100)) +
                "% (" + std::to_string(static_cast<uint32_t>(_prev_predicted_bw)) +
                " -> " + std::to_string(bw) + " bytes/s)");
        }
    } else if (_state == AgentState::DEGRADED && _pre_degraded_bw > 0) {
        // 检测带宽恢复：相对于骤降前的水平恢复到 85%
        if (static_cast<float>(bw) >= _pre_degraded_bw * BW_RECOVER_RATIO) {
            transition_to(AgentState::NORMAL,
                "bandwidth recovered to " + std::to_string(bw) + " bytes/s");
        }
    }
}

// ============================================================================
// effective_alpha - 经过状态 clamp 后的 alpha
// ============================================================================
float NetworkAgent::effective_alpha() const {
    switch (_state) {
        case AgentState::NORMAL:
            return std::clamp(_current_alpha, -1.0f, 1.0f);
        case AgentState::DEGRADED:
            return std::clamp(_current_alpha, -1.0f, 0.3f);
        case AgentState::UNDER_ATTACK:
            return 0.0f;  // 不使用 Orca，切回 Cubic
        case AgentState::RECOVERY:
            return std::clamp(_current_alpha, -1.0f, 0.5f);
    }
    return 0.0f;
}

// ============================================================================
// should_accept_connection - 是否允许新连接
// ============================================================================
bool NetworkAgent::should_accept_connection() const {
    switch (_state) {
        case AgentState::NORMAL:
        case AgentState::DEGRADED:
            return true;    // 正常和带宽降级时不限制连接
        case AgentState::RECOVERY:
            return true;    // 恢复期允许连接
        case AgentState::UNDER_ATTACK:
            return false;   // 攻击期间拒绝新连接
    }
    return true;
}

// ============================================================================
// transition_to - 执行状态转移并记录日志
// ============================================================================
void NetworkAgent::transition_to(AgentState new_state, const std::string& reason) {
    if (_state == new_state) return;

    AgentState old_state = _state;
    _state = new_state;

    // 进入新状态时初始化相关计数器
    if (new_state == AgentState::RECOVERY) {
        _recovery_ticks = 0;
        _anomaly_clear_count = 0;
    }

    // 记录决策
    _history.emplace_back(get_now_us(), old_state, new_state, reason);
    if (_history.size() > MAX_HISTORY_SIZE) {
        _history.pop_front();
    }
}

// ============================================================================
// get_now_us - 获取当前时间戳
// ============================================================================
uint64_t NetworkAgent::get_now_us() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();
}

} // namespace neustack
