/**
 * AI Model Validation Test
 *
 * Tests each ONNX model independently with properly normalized inputs
 * matching the Python training normalization, then validates output ranges.
 *
 * 1. test_orca_model()      - 7-dim input, verify alpha in [-1,1]
 * 2. test_bandwidth_model() - 30x3 time series, verify predicted_bw
 * 3. test_anomaly_model()   - normal vs anomalous, compare reconstruction errors
 * 4. test_integration()     - full pipeline via IntelligencePlane
 */

#include "neustack/ai/intelligence_plane.hpp"
#include "neustack/ai/orca_model.hpp"
#include "neustack/ai/anomaly_model.hpp"
#include "neustack/ai/bandwidth_model.hpp"
#include "neustack/ai/ai_agent.hpp"
#include "neustack/metrics/ai_features.hpp"
#include "neustack/common/log.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>
#include <cassert>

using namespace neustack;

namespace {

std::string repo_model_path(const char* relative_path) {
    return std::string(NEUSTACK_SOURCE_DIR) + "/" + relative_path;
}

} // namespace

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        std::cout << "  PASS: " << msg << std::endl; \
        tests_passed++; \
    } else { \
        std::cout << "  FAIL: " << msg << std::endl; \
        tests_failed++; \
    } \
} while(0)

// ============================================================================
// Test 1: Orca model with correctly normalized inputs
// ============================================================================
bool test_orca_model() {
    std::cout << "\n=== Test 1: Orca Model ===" << std::endl;

    OrcaModel model(repo_model_path("models/orca_actor.onnx"));
    if (!model.is_loaded()) {
        std::cout << "  SKIP: model not loaded" << std::endl;
        return false;
    }
    std::cout << "  Model loaded successfully" << std::endl;

    // Scenario A: Normal traffic
    // delivery_rate ~= est_bw, low queuing delay, low loss
    {
        ICongestionModel::Input input{
            .throughput_normalized = 0.9f,   // delivery ~90% of est_bw, clip[0,2]
            .queuing_delay_normalized = 0.1f, // low queuing delay, clip[0,5]
            .rtt_ratio = 1.1f,                // rtt slightly above min_rtt, clip[1,5]
            .loss_rate = 0.0f,                // no loss, clip[0,1]
            .cwnd_normalized = 1.0f,          // cwnd = BDP, clip[0,10]
            .in_flight_ratio = 0.8f,          // 80% of cwnd in flight, clip[0,2]
            .predicted_bw_normalized = 0.5f   // predicted_bw / MAX_BW, clip[0,2]
        };

        auto result = model.infer(input);
        CHECK(result.has_value(), "normal traffic: got result");
        if (result) {
            std::cout << "    alpha = " << result->alpha << std::endl;
            CHECK(result->alpha >= -1.0f && result->alpha <= 1.0f,
                  "normal traffic: alpha in [-1,1]");
        }
    }

    // Scenario B: Congestion
    // throughput dropping, high queuing delay, high RTT, loss present
    float alpha_normal = 0.0f, alpha_congested = 0.0f;
    {
        ICongestionModel::Input input{
            .throughput_normalized = 0.3f,    // throughput dropped
            .queuing_delay_normalized = 3.0f, // high queuing delay
            .rtt_ratio = 3.5f,                // high RTT ratio
            .loss_rate = 0.1f,                // 10% loss
            .cwnd_normalized = 0.5f,          // cwnd below BDP
            .in_flight_ratio = 1.5f,          // over-sending
            .predicted_bw_normalized = 0.3f
        };

        auto result = model.infer(input);
        CHECK(result.has_value(), "congested: got result");
        if (result) {
            alpha_congested = result->alpha;
            std::cout << "    alpha = " << result->alpha << std::endl;
            CHECK(result->alpha >= -1.0f && result->alpha <= 1.0f,
                  "congested: alpha in [-1,1]");
        }
    }

    // Re-run normal to compare
    {
        ICongestionModel::Input input{
            .throughput_normalized = 0.9f,
            .queuing_delay_normalized = 0.1f,
            .rtt_ratio = 1.1f,
            .loss_rate = 0.0f,
            .cwnd_normalized = 1.0f,
            .in_flight_ratio = 0.8f,
            .predicted_bw_normalized = 0.5f
        };
        auto result = model.infer(input);
        if (result) alpha_normal = result->alpha;
    }

    CHECK(alpha_normal != alpha_congested,
          "different inputs produce different outputs");
    std::cout << "    normal_alpha=" << alpha_normal
              << " congested_alpha=" << alpha_congested << std::endl;

    // Scenario C: Using OrcaFeatures::from_sample() to verify feature construction
    {
        TCPSample sample{};
        sample.timestamp_us = 1000000;
        sample.rtt_us = 22000;       // 22ms
        sample.min_rtt_us = 20000;   // 20ms baseline
        sample.srtt_us = 21000;
        sample.cwnd = 100;           // 100 MSS
        sample.bytes_in_flight = 100000;
        sample.delivery_rate = 5000000;  // 5 MB/s
        sample.packets_sent = 200;
        sample.packets_lost = 0;

        uint32_t est_bw = 6000000;    // 6 MB/s
        float predicted_bw = 5500000;  // 5.5 MB/s

        auto features = OrcaFeatures::from_sample(sample, est_bw, predicted_bw);
        auto vec = features.to_vector();

        CHECK(vec.size() == 7, "from_sample produces 7-dim vector");
        std::cout << "    from_sample: [";
        for (size_t i = 0; i < vec.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << vec[i];
        }
        std::cout << "]" << std::endl;

        // Validate ranges match training clipping
        CHECK(vec[0] >= 0.0f && vec[0] <= 2.0f, "throughput_norm in [0,2]");
        CHECK(vec[1] >= 0.0f && vec[1] <= 5.0f, "queuing_delay_norm in [0,5]");
        CHECK(vec[2] >= 1.0f && vec[2] <= 5.0f, "rtt_ratio in [1,5]");
        CHECK(vec[3] >= 0.0f && vec[3] <= 1.0f, "loss_rate in [0,1]");
        CHECK(vec[4] >= 0.0f && vec[4] <= 10.0f, "cwnd_norm in [0,10]");
        CHECK(vec[5] >= 0.0f && vec[5] <= 2.0f, "in_flight_ratio in [0,2]");
        CHECK(vec[6] >= 0.0f && vec[6] <= 2.0f, "predicted_bw_norm in [0,2]");

        // Feed through model
        ICongestionModel::Input input{
            .throughput_normalized = vec[0],
            .queuing_delay_normalized = vec[1],
            .rtt_ratio = vec[2],
            .loss_rate = vec[3],
            .cwnd_normalized = vec[4],
            .in_flight_ratio = vec[5],
            .predicted_bw_normalized = vec[6]
        };
        auto result = model.infer(input);
        CHECK(result.has_value(), "from_sample input: got result");
        if (result) {
            std::cout << "    alpha = " << result->alpha << std::endl;
        }
    }

    return true;
}

// ============================================================================
// Test 2: Bandwidth predictor with 30-step time series
// ============================================================================
bool test_bandwidth_model() {
    std::cout << "\n=== Test 2: Bandwidth Predictor ===" << std::endl;

    BandwidthPredictor model(repo_model_path("models/bandwidth_predictor.onnx"));
    if (!model.is_loaded()) {
        std::cout << "  SKIP: model not loaded" << std::endl;
        return false;
    }
    std::cout << "  Model loaded, required_history=" << model.required_history_length() << std::endl;

    size_t hist_len = model.required_history_length();

    // Scenario A: Stable traffic at ~5 MB/s
    {
        IBandwidthModel::Input input;
        input.throughput_history.resize(hist_len);
        input.rtt_history.resize(hist_len);
        input.loss_history.resize(hist_len);

        for (size_t i = 0; i < hist_len; i++) {
            // throughput = delivery_rate / MAX_BW(10MB/s), clip[0,1]
            input.throughput_history[i] = 0.5f;  // 5 MB/s / 10 MB/s
            // rtt_ratio = rtt / min_rtt, clip[1,5]
            input.rtt_history[i] = 1.1f;
            // loss_rate, clip[0,1]
            input.loss_history[i] = 0.0f;
        }

        auto result = model.infer(input);
        CHECK(result.has_value(), "stable traffic: got result");
        if (result) {
            std::cout << "    predicted_bw = " << result->predicted_bandwidth
                      << " bytes/s, confidence = " << result->confidence << std::endl;
            CHECK(result->predicted_bandwidth > 0,
                  "stable traffic: predicted_bw > 0");
            CHECK(result->predicted_bandwidth <= 10000000,
                  "stable traffic: predicted_bw <= MAX_BW(10MB/s)");
        }
    }

    // Scenario B: Declining traffic (congestion onset)
    {
        IBandwidthModel::Input input;
        input.throughput_history.resize(hist_len);
        input.rtt_history.resize(hist_len);
        input.loss_history.resize(hist_len);

        for (size_t i = 0; i < hist_len; i++) {
            float t = static_cast<float>(i) / hist_len;
            // Throughput declining from 0.8 to 0.2
            input.throughput_history[i] = std::clamp(0.8f - 0.6f * t, 0.0f, 1.0f);
            // RTT increasing from 1.2 to 3.0
            input.rtt_history[i] = std::clamp(1.2f + 1.8f * t, 1.0f, 5.0f);
            // Loss increasing from 0 to 0.05
            input.loss_history[i] = std::clamp(0.05f * t, 0.0f, 1.0f);
        }

        auto result = model.infer(input);
        CHECK(result.has_value(), "declining traffic: got result");
        if (result) {
            std::cout << "    predicted_bw = " << result->predicted_bandwidth
                      << " bytes/s, confidence = " << result->confidence << std::endl;
        }
    }

    // Scenario C: Using BandwidthFeatures::from_samples()
    {
        std::vector<TCPSample> samples(hist_len);
        uint32_t min_rtt = 20000;

        for (size_t i = 0; i < hist_len; i++) {
            samples[i].timestamp_us = i * 100000;
            samples[i].delivery_rate = 5000000;    // 5 MB/s
            samples[i].rtt_us = 22000;             // 22ms
            samples[i].min_rtt_us = min_rtt;       // 20ms
            samples[i].packets_sent = 200;
            samples[i].packets_lost = 0;
        }

        auto features = BandwidthFeatures::from_samples(samples, min_rtt);

        CHECK(features.throughput_history.size() == hist_len, "from_samples: correct length");
        std::cout << "    from_samples first step: throughput="
                  << features.throughput_history[0]
                  << " rtt=" << features.rtt_history[0]
                  << " loss=" << features.loss_history[0] << std::endl;

        // Validate ranges
        CHECK(features.throughput_history[0] >= 0.0f && features.throughput_history[0] <= 1.0f,
              "throughput in [0,1]");
        CHECK(features.rtt_history[0] >= 1.0f && features.rtt_history[0] <= 5.0f,
              "rtt_ratio in [1,5]");

        // Feed through model
        IBandwidthModel::Input input;
        input.throughput_history = features.throughput_history;
        input.rtt_history = features.rtt_history;
        input.loss_history = features.loss_history;

        auto result = model.infer(input);
        CHECK(result.has_value(), "from_samples input: got result");
        if (result) {
            std::cout << "    predicted_bw = " << result->predicted_bandwidth
                      << " bytes/s" << std::endl;
        }
    }

    return true;
}

// ============================================================================
// Test 3: Anomaly detector - normal vs anomalous
// ============================================================================
bool test_anomaly_model() {
    std::cout << "\n=== Test 3: Anomaly Detector ===" << std::endl;

    AnomalyDetector model(repo_model_path("models/anomaly_detector.onnx"));
    if (!model.is_loaded()) {
        std::cout << "  SKIP: model not loaded" << std::endl;
        return false;
    }
    std::cout << "  Model loaded, threshold=" << model.threshold() << std::endl;

    // Scenario A: Normal traffic
    float normal_error = 0.0f;
    {
        // 8-dim ratio-based input matching Python training normalization
        IAnomalyModel::Input input{
            .log_pkt_rate = 0.5f,        // moderate packet rate
            .bytes_per_pkt = 0.6f,       // typical ~900B packets
            .syn_ratio = 0.005f,         // very low SYN fraction
            .rst_ratio = 0.002f,         // very low RST fraction
            .conn_completion = 0.95f,    // most connections complete
            .tx_rx_ratio = 0.45f,        // roughly balanced
            .log_active_conn = 0.35f,    // moderate connections
            .log_conn_reset = 0.01f         // very few resets
        };

        auto result = model.infer(input);
        CHECK(result.has_value(), "normal traffic: got result");
        if (result) {
            normal_error = result->reconstruction_error;
            std::cout << "    reconstruction_error = " << result->reconstruction_error
                      << ", is_anomaly = " << result->is_anomaly << std::endl;
        }
    }

    // Scenario B: SYN flood (anomalous)
    float anomaly_error = 0.0f;
    {
        IAnomalyModel::Input input{
            .log_pkt_rate = 0.5f,        // moderate packet rate
            .bytes_per_pkt = 0.03f,      // tiny SYN packets (~40B)
            .syn_ratio = 0.6f,           // high SYN fraction (flood!)
            .rst_ratio = 0.01f,          // low RST
            .conn_completion = 0.05f,    // almost no connections complete
            .tx_rx_ratio = 0.2f,         // asymmetric (receiving flood)
            .log_active_conn = 0.7f,     // elevated half-open connections
            .log_conn_reset = 0.01f         // low resets
        };

        auto result = model.infer(input);
        CHECK(result.has_value(), "syn flood: got result");
        if (result) {
            anomaly_error = result->reconstruction_error;
            std::cout << "    reconstruction_error = " << result->reconstruction_error
                      << ", is_anomaly = " << result->is_anomaly << std::endl;
        }
    }

    CHECK(anomaly_error > normal_error,
          "anomalous input has higher reconstruction error than normal");
    std::cout << "    normal_error=" << normal_error
              << " anomaly_error=" << anomaly_error << std::endl;

    return true;
}

// ============================================================================
// Test 4: Integration test via IntelligencePlane
// ============================================================================
bool test_integration() {
    std::cout << "\n=== Test 4: Integration (IntelligencePlane) ===" << std::endl;

    MetricsBuffer<TCPSample, 1024> metrics_buf;
    SPSCQueue<AIAction, 16> action_queue;

    IntelligencePlaneConfig config;
    config.orca_model_path = repo_model_path("models/orca_actor.onnx");
    config.anomaly_model_path = repo_model_path("models/anomaly_detector.onnx");
    config.bandwidth_model_path = repo_model_path("models/bandwidth_predictor.onnx");
    config.orca_interval = std::chrono::milliseconds(50);
    config.anomaly_interval = std::chrono::milliseconds(200);
    config.bandwidth_interval = std::chrono::milliseconds(100);

    IntelligencePlane ai(metrics_buf, action_queue, config);
    ai.start();

    CHECK(ai.is_running(), "intelligence plane started");

    // Push enough samples for bandwidth predictor (requires 30)
    for (int i = 0; i < 40; i++) {
        TCPSample sample{};
        sample.timestamp_us = i * 10000;
        sample.min_rtt_us = 20000;
        sample.srtt_us = 21000;
        sample.rtt_us = 22000 + (i % 3) * 500;
        sample.delivery_rate = 5000000;  // 5 MB/s
        sample.cwnd = 80;
        sample.bytes_in_flight = 80000;
        sample.packets_sent = 200;
        sample.packets_lost = 0;

        metrics_buf.push(sample);

        if (i % 10 == 9) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    }

    // Wait for inference cycles
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Collect actions
    int cwnd_count = 0, bw_count = 0, anomaly_count = 0;
    AIAction action;
    while (action_queue.try_pop(action)) {
        switch (action.type) {
            case AIAction::Type::CWND_ADJUST:
                cwnd_count++;
                std::cout << "  [CWND] alpha=" << action.cwnd.alpha << std::endl;
                CHECK(action.cwnd.alpha >= -1.0f && action.cwnd.alpha <= 1.0f,
                      "integration: alpha in [-1,1]");
                break;
            case AIAction::Type::BW_PREDICTION:
                bw_count++;
                std::cout << "  [BW_PRED] predicted=" << action.bandwidth.predicted_bw
                          << " bytes/s" << std::endl;
                break;
            case AIAction::Type::ANOMALY_ALERT:
                anomaly_count++;
                std::cout << "  [ANOMALY] score=" << action.anomaly.score << std::endl;
                break;
            default:
                break;
        }
    }

    ai.stop();

    std::cout << "  Actions: cwnd=" << cwnd_count << " bw=" << bw_count
              << " anomaly=" << anomaly_count << std::endl;

    CHECK(cwnd_count > 0, "integration: received CWND actions");

    return true;
}

// ============================================================================
// Test 5: NetworkAgent state machine
// ============================================================================
bool test_network_agent() {
    std::cout << "\n=== Test 5: NetworkAgent ===" << std::endl;

    NetworkAgent agent(0.01f);  // threshold = 0.01

    // 1. 初始状态
    CHECK(agent.state() == AgentState::NORMAL, "initial state is NORMAL");
    CHECK(agent.should_accept_connection(), "NORMAL accepts connections");

    // 2. 正常 anomaly score 不触发状态变化
    agent.on_anomaly(0.001f);
    agent.on_anomaly(0.005f);
    CHECK(agent.state() == AgentState::NORMAL, "low scores stay NORMAL");

    // 3. 高 anomaly score → UNDER_ATTACK
    agent.on_anomaly(0.2f);
    CHECK(agent.state() == AgentState::UNDER_ATTACK, "high score → UNDER_ATTACK");
    CHECK(!agent.should_accept_connection(), "UNDER_ATTACK rejects connections");
    CHECK(agent.effective_alpha() == 0.0f, "UNDER_ATTACK: alpha forced to 0");
    std::cout << "    state=" << agent_state_name(agent.state()) << std::endl;

    // 4. 单次正常不够恢复
    agent.on_anomaly(0.001f);
    CHECK(agent.state() == AgentState::UNDER_ATTACK, "one clear not enough");

    // 5. 连续 50 次正常 → RECOVERY
    for (int i = 0; i < 50; i++) {
        agent.on_anomaly(0.001f);
    }
    CHECK(agent.state() == AgentState::RECOVERY, "50 clears → RECOVERY");
    CHECK(agent.should_accept_connection(), "RECOVERY accepts connections");
    std::cout << "    state=" << agent_state_name(agent.state()) << std::endl;

    // 6. RECOVERY 中 alpha 被 clamp 到 [-1, 0.5]
    agent.on_cwnd_adjust(0.8f);
    CHECK(agent.effective_alpha() <= 0.5f, "RECOVERY: alpha clamped to 0.5");
    std::cout << "    effective_alpha=" << agent.effective_alpha() << std::endl;

    // 7. RECOVERY tick 倒计时 → NORMAL
    for (int i = 0; i < 100; i++) {
        agent.on_cwnd_adjust(0.1f);
    }
    CHECK(agent.state() == AgentState::NORMAL, "100 ticks → NORMAL");
    std::cout << "    state=" << agent_state_name(agent.state()) << std::endl;

    // 8. 攻击期间再次异常重置计数器
    agent.on_anomaly(0.5f);  // → UNDER_ATTACK
    for (int i = 0; i < 40; i++) agent.on_anomaly(0.001f);  // 快接近 50
    agent.on_anomaly(0.5f);  // 突然又异常，重置计数
    for (int i = 0; i < 30; i++) agent.on_anomaly(0.001f);  // 才 30 次
    CHECK(agent.state() == AgentState::UNDER_ATTACK, "re-attack resets counter");

    // 9. 带宽骤降 → DEGRADED
    // 先恢复到 NORMAL
    for (int i = 0; i < 50; i++) agent.on_anomaly(0.001f);  // → RECOVERY
    for (int i = 0; i < 100; i++) agent.on_cwnd_adjust(0.1f);  // → NORMAL
    CHECK(agent.state() == AgentState::NORMAL, "back to NORMAL for BW test");

    agent.on_bw_prediction(5000000);  // 建立基线 5MB/s
    agent.on_bw_prediction(2000000);  // 骤降 60% → DEGRADED
    CHECK(agent.state() == AgentState::DEGRADED, "BW drop → DEGRADED");
    std::cout << "    state=" << agent_state_name(agent.state()) << std::endl;

    // 10. DEGRADED 时 alpha 被 clamp 到 [-1, 0.3]
    agent.on_cwnd_adjust(0.9f);
    CHECK(agent.effective_alpha() <= 0.3f, "DEGRADED: alpha clamped to 0.3");

    // 11. 带宽恢复 → NORMAL
    agent.on_bw_prediction(4500000);  // 恢复到基线 85% 以上
    CHECK(agent.state() == AgentState::NORMAL, "BW recovered → NORMAL");

    // 12. 检查决策日志
    auto& history = agent.history();
    CHECK(history.size() > 0, "history has entries");
    std::cout << "    decision history (" << history.size() << " entries):" << std::endl;
    for (const auto& d : history) {
        std::cout << "      " << agent_state_name(d.from_state) << " → "
                  << agent_state_name(d.to_state) << ": " << d.reason << std::endl;
    }

    return true;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "=== AI Model Validation Test ===" << std::endl;

    test_orca_model();
    test_bandwidth_model();
    test_anomaly_model();
    test_integration();
    test_network_agent();

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
