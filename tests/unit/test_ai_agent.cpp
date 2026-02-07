#include <catch2/catch_test_macros.hpp>
#include "neustack/ai/ai_agent.hpp"
#include <deque>
#include <vector>

using namespace neustack;

TEST_CASE("NetworkAgent state machine and alpha control", "[ai][agent]") {
    NetworkAgent agent(0.01f); // Threshold = 0.01

    SECTION("Initial state") {
        CHECK(agent.state() == AgentState::NORMAL);
        CHECK(agent.should_accept_connection() == true);
        CHECK(agent.effective_alpha() == 0.0f); // Default alpha is 0
    }

    SECTION("Anomaly detection: UNDER_ATTACK transitions") {
        // Low score does nothing
        agent.on_anomaly(0.005f);
        CHECK(agent.state() == AgentState::NORMAL);

        // High score triggers UNDER_ATTACK
        agent.on_anomaly(0.02f);
        CHECK(agent.state() == AgentState::UNDER_ATTACK);
        CHECK(agent.should_accept_connection() == false);

        SECTION("Clamping in UNDER_ATTACK") {
            agent.on_cwnd_adjust(0.8f);
            CHECK(agent.effective_alpha() == 0.0f);
            agent.on_cwnd_adjust(-0.5f);
            CHECK(agent.effective_alpha() == 0.0f);
        }

        SECTION("Recovery from UNDER_ATTACK via clear counter") {
            // 49 consecutive low scores
            for (int i = 0; i < 49; ++i) {
                agent.on_anomaly(0.001f);
                CHECK(agent.state() == AgentState::UNDER_ATTACK);
            }

            // Interrupt with one high score
            agent.on_anomaly(0.05f);
            
            // Should now need another full 50 rounds
            for (int i = 0; i < 49; ++i) {
                agent.on_anomaly(0.001f);
            }
            CHECK(agent.state() == AgentState::UNDER_ATTACK);

            // 50th low score triggers RECOVERY
            agent.on_anomaly(0.001f);
            CHECK(agent.state() == AgentState::RECOVERY);
        }
    }

    SECTION("Recovery phase and ticks") {
        agent.on_anomaly(0.1f); // To UNDER_ATTACK
        for (int i = 0; i < 50; ++i) agent.on_anomaly(0.0f); // To RECOVERY
        
        REQUIRE(agent.state() == AgentState::RECOVERY);
        CHECK(agent.should_accept_connection() == true);

        SECTION("Clamping in RECOVERY") {
            agent.on_cwnd_adjust(0.8f);
            CHECK(agent.effective_alpha() == 0.5f); // Clamped to 0.5
            agent.on_cwnd_adjust(-0.2f);
            CHECK(agent.effective_alpha() == -0.2f); // Negative not clamped
        }

        SECTION("Transition back to NORMAL") {
            for (int i = 0; i < 99; ++i) {
                agent.on_cwnd_adjust(0.1f);
                CHECK(agent.state() == AgentState::RECOVERY);
            }
            agent.on_cwnd_adjust(0.1f); // 100th tick
            CHECK(agent.state() == AgentState::NORMAL);
        }
    }

    SECTION("Bandwidth-based degradation") {
        agent.on_bw_prediction(5000000); // Base BW
        CHECK(agent.state() == AgentState::NORMAL);

        // Drop to 2,000,000 (60% drop, > 50% threshold)
        agent.on_bw_prediction(2000000);
        CHECK(agent.state() == AgentState::DEGRADED);

        SECTION("Clamping in DEGRADED") {
            agent.on_cwnd_adjust(0.9f);
            CHECK(agent.effective_alpha() == 0.3f); // Max 0.3
        }

        SECTION("Recovery to NORMAL via BW increase") {
            // Need 85% of 5M = 4,250,000
            agent.on_bw_prediction(4000000);
            CHECK(agent.state() == AgentState::DEGRADED);

            agent.on_bw_prediction(4250000);
            CHECK(agent.state() == AgentState::NORMAL);
        }
    }

    SECTION("Decision history tracking") {
        agent.on_anomaly(0.1f); // NORMAL -> UNDER_ATTACK
        
        const auto& history = agent.history();
        REQUIRE_FALSE(history.empty());
        CHECK(history.back().from_state == AgentState::NORMAL);
        CHECK(history.back().to_state == AgentState::UNDER_ATTACK);
    }

    SECTION("History buffer capacity") {
        // Force many transitions or just check max size logic
        // Since history only records state transitions, we trigger them repeatedly
        for (int i = 0; i < 600; ++i) {
            agent.on_anomaly(0.1f);
            for(int j=0; j<50; ++j) agent.on_anomaly(0.0f);
            for(int k=0; k<100; ++k) agent.on_cwnd_adjust(0.0f);
        }
        
        CHECK(agent.history().size() <= 1000);
    }
}