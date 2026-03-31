#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "neustack/telemetry/telemetry_api.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "../../helpers/json_validator.hpp"

#include <thread>
#include <chrono>

using namespace neustack::telemetry;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::ContainsSubstring;

// ════════════════════════════════════════════
// 测试辅助: 构造一个最小化的 TelemetryAPI
// ════════════════════════════════════════════

namespace {

struct TestEnv {
    neustack::GlobalMetrics gm{};
    neustack::SecurityMetrics sm;
    neustack::TCPConnectionManager tcp_mgr{0x0A000001}; // 10.0.0.1
    MetricsRegistry registry;
    std::chrono::steady_clock::time_point start_time;

    std::unique_ptr<TelemetryAPI> api;

    TestEnv() : start_time(std::chrono::steady_clock::now()) {
        // 注册 RTT histogram (TelemetryAPI 内部查找这个名字)
        registry.histogram("neustack_tcp_rtt_us",
            "TCP round-trip time distribution",
            {50, 100, 200, 500, 1000, 2000, 5000, 10000, 50000, 100000, 500000},
            "microseconds");

        api = std::make_unique<TelemetryAPI>(
            gm, &sm, tcp_mgr,
            nullptr,  // no firewall
            nullptr,  // no AI agent
            registry,
            start_time);
    }
};

} // anonymous namespace

// ════════════════════════════════════════════
// StackStatus 默认值
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: Status initial values are zero", "[telemetry][api]") {
    TestEnv env;
    auto s = env.api->status();

    CHECK(s.uptime_seconds == 0); // 刚构造，uptime ~0
    CHECK(s.traffic.packets_rx == 0);
    CHECK(s.traffic.packets_tx == 0);
    CHECK(s.traffic.bytes_rx == 0);
    CHECK(s.traffic.bytes_tx == 0);
    CHECK_THAT(s.traffic.pps_rx, WithinAbs(0.0, 1e-9));
    CHECK_THAT(s.traffic.pps_tx, WithinAbs(0.0, 1e-9));
    CHECK(s.tcp.active_connections == 0);
    CHECK(s.tcp.total_established == 0);
    CHECK(s.tcp.total_reset == 0);
    CHECK(s.tcp.total_timeout == 0);
    CHECK(s.tcp.rtt.samples == 0);
    CHECK_THAT(s.tcp.avg_cwnd, WithinAbs(0.0, 1e-9));
}

// ════════════════════════════════════════════
// Traffic 子查询
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: Traffic counters reflect GlobalMetrics", "[telemetry][api]") {
    TestEnv env;

    env.gm.packets_rx.store(1000, std::memory_order_relaxed);
    env.gm.packets_tx.store(800, std::memory_order_relaxed);
    env.gm.bytes_rx.store(500000, std::memory_order_relaxed);
    env.gm.bytes_tx.store(400000, std::memory_order_relaxed);

    auto t = env.api->traffic();

    CHECK(t.packets_rx == 1000);
    CHECK(t.packets_tx == 800);
    CHECK(t.bytes_rx == 500000);
    CHECK(t.bytes_tx == 400000);
}

TEST_CASE("TelemetryAPI: Rate calculation first call returns zero", "[telemetry][api]") {
    TestEnv env;

    env.gm.packets_rx.store(100, std::memory_order_relaxed);
    auto t = env.api->traffic();

    // 首次调用, 速率应为 0
    CHECK_THAT(t.pps_rx, WithinAbs(0.0, 1e-9));
    CHECK_THAT(t.pps_tx, WithinAbs(0.0, 1e-9));
    CHECK_THAT(t.bps_rx, WithinAbs(0.0, 1e-9));
    CHECK_THAT(t.bps_tx, WithinAbs(0.0, 1e-9));
}

TEST_CASE("TelemetryAPI: Rate calculation after interval", "[telemetry][api]") {
    TestEnv env;

    // 第一次调用 (建立基线)
    env.gm.packets_rx.store(1000, std::memory_order_relaxed);
    env.gm.bytes_rx.store(500000, std::memory_order_relaxed);
    env.api->update_rates();

    // 等待一小段时间使 dt > 0.001
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 更新计数器
    env.gm.packets_rx.store(2000, std::memory_order_relaxed);
    env.gm.bytes_rx.store(1000000, std::memory_order_relaxed);

    auto t = env.api->traffic();

    // 速率应该 > 0 (具体值取决于实际时间间隔)
    CHECK(t.pps_rx > 0.0);
    CHECK(t.bps_rx > 0.0);
}

// ════════════════════════════════════════════
// TCP 子查询
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: TCP stats from GlobalMetrics", "[telemetry][api]") {
    TestEnv env;

    env.gm.active_connections.store(42, std::memory_order_relaxed);
    env.gm.conn_established.store(100, std::memory_order_relaxed);
    env.gm.conn_reset.store(5, std::memory_order_relaxed);
    env.gm.conn_timeout.store(2, std::memory_order_relaxed);
    env.gm.total_retransmits.store(9, std::memory_order_relaxed);

    auto tcp = env.api->tcp_stats();

    CHECK(tcp.active_connections == 42);
    CHECK(tcp.total_established == 100);
    CHECK(tcp.total_reset == 5);
    CHECK(tcp.total_timeout == 2);
    CHECK(tcp.total_retransmits == 9);
}

TEST_CASE("TelemetryAPI: TCP RTT from Histogram", "[telemetry][api]") {
    TestEnv env;

    // 向 RTT histogram 注入数据
    auto* rtt_hist = env.registry.find_histogram("neustack_tcp_rtt_us");
    REQUIRE(rtt_hist != nullptr);

    // 100 个样本: 100us, 200us, ..., 10000us
    for (int i = 1; i <= 100; ++i) {
        rtt_hist->observe(static_cast<double>(i * 100));
    }

    auto tcp = env.api->tcp_stats();

    CHECK(tcp.rtt.samples == 100);
    // avg = sum(100..10000 step 100) / 100 = 505000 / 100 = 5050
    CHECK_THAT(tcp.rtt.avg_us, WithinAbs(5050.0, 1.0));
    // percentile 应在合理范围
    CHECK(tcp.rtt.p50_us > 0.0);
    CHECK(tcp.rtt.p50_us < tcp.rtt.p99_us);
    CHECK(tcp.rtt.p90_us > tcp.rtt.p50_us);
    CHECK(tcp.rtt.p99_us > tcp.rtt.p90_us);
}

TEST_CASE("TelemetryAPI: TCP avg_cwnd is zero with no connections", "[telemetry][api]") {
    TestEnv env;
    auto tcp = env.api->tcp_stats();
    CHECK_THAT(tcp.avg_cwnd, WithinAbs(0.0, 1e-9));
}

// ════════════════════════════════════════════
// Security 子查询
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: Security stats without firewall/agent", "[telemetry][api]") {
    TestEnv env;
    auto sec = env.api->security_stats();

    CHECK(sec.firewall_enabled == false);
    CHECK(sec.ai_enabled == false);
    CHECK(sec.agent_state == "DISABLED");
    CHECK_THAT(sec.anomaly_score, WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(sec.predicted_bandwidth_bps, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("TelemetryAPI: Security stats with SecurityMetrics", "[telemetry][api]") {
    TestEnv env;

    // 模拟流量
    for (int i = 0; i < 100; ++i) {
        env.sm.record_packet(512, 0x10); // ACK
    }
    for (int i = 0; i < 10; ++i) {
        env.sm.record_packet(60, 0x02);  // SYN
    }
    env.sm.tick();

    auto sec = env.api->security_stats();
    CHECK(sec.pps > 0.0);
    CHECK(sec.syn_rate > 0.0);
}

// ════════════════════════════════════════════
// 完整 status 快照
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: Full status snapshot", "[telemetry][api]") {
    TestEnv env;

    env.gm.packets_rx.store(5000, std::memory_order_relaxed);
    env.gm.active_connections.store(10, std::memory_order_relaxed);

    auto s = env.api->status();

    CHECK(s.traffic.packets_rx == 5000);
    CHECK(s.tcp.active_connections == 10);
    CHECK(s.ai.enabled == false);
    CHECK(s.ai.orca_status == "disabled");
    CHECK(s.ai.anomaly_status == "disabled");
    CHECK(s.ai.bandwidth_status == "disabled");
}

TEST_CASE("TelemetryAPI: Uptime increases", "[telemetry][api]") {
    // 用一个过去的时间点作为 start_time
    auto past = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    neustack::GlobalMetrics gm{};
    neustack::TCPConnectionManager tcp_mgr{0x0A000001};
    MetricsRegistry registry;

    TelemetryAPI api(gm, nullptr, tcp_mgr, nullptr, nullptr, registry, past);
    auto s = api.status();

    CHECK(s.uptime_seconds >= 5);
}

// ════════════════════════════════════════════
// 连接列表
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: Empty connections list", "[telemetry][api]") {
    TestEnv env;
    auto conns = env.api->connections();
    CHECK(conns.empty());
}

TEST_CASE("TelemetryAPI: connections_by_ip with no match", "[telemetry][api]") {
    TestEnv env;
    auto conns = env.api->connections_by_ip(0xC0A80001); // 192.168.0.1
    CHECK(conns.empty());
}

TEST_CASE("TelemetryAPI: connections_by_port with no match", "[telemetry][api]") {
    TestEnv env;
    auto conns = env.api->connections_by_port(8080);
    CHECK(conns.empty());
}

// ════════════════════════════════════════════
// 导出
// ════════════════════════════════════════════

TEST_CASE("TelemetryAPI: to_json returns valid JSON", "[telemetry][api]") {
    TestEnv env;

    // 注册一些指标让输出非空
    env.registry.counter("test_c", "Test counter");
    env.registry.gauge("test_g", "Test gauge");

    auto json = env.api->to_json();
    CHECK(!json.empty());
    CHECK(neustack::test::JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"timestamp\""));
    CHECK_THAT(json, ContainsSubstring("\"metrics\""));
}

TEST_CASE("TelemetryAPI: to_json pretty mode", "[telemetry][api]") {
    TestEnv env;
    env.registry.counter("test_c", "Test counter");

    auto compact = env.api->to_json(false);
    auto pretty = env.api->to_json(true);

    CHECK(pretty.size() > compact.size());
    CHECK_THAT(pretty, ContainsSubstring("\n"));
}

TEST_CASE("TelemetryAPI: to_prometheus returns valid format", "[telemetry][api]") {
    TestEnv env;

    auto& c = env.registry.counter("test_prom_counter", "Prometheus test");
    c.increment(42);

    auto prom = env.api->to_prometheus();
    CHECK(!prom.empty());
    CHECK_THAT(prom, ContainsSubstring("# HELP test_prom_counter"));
    CHECK_THAT(prom, ContainsSubstring("# TYPE test_prom_counter counter"));
    CHECK_THAT(prom, ContainsSubstring("test_prom_counter 42"));
}

// ════════════════════════════════════════════
// ConnectionDetail 结构体
// ════════════════════════════════════════════

TEST_CASE("ConnectionDetail: Default initialization", "[telemetry][api]") {
    ConnectionDetail d{};
    CHECK(d.local_ip == 0);
    CHECK(d.local_port == 0);
    CHECK(d.remote_ip == 0);
    CHECK(d.remote_port == 0);
    CHECK(d.state.empty());
    CHECK(d.rtt_us == 0);
    CHECK(d.srtt_us == 0);
    CHECK(d.cwnd == 0);
    CHECK(d.bytes_in_flight == 0);
    CHECK(d.send_buffer_used == 0);
    CHECK(d.recv_buffer_used == 0);
    CHECK(d.bytes_sent == 0);
    CHECK(d.bytes_received == 0);
}

// ════════════════════════════════════════════
// StackStatus 结构体
// ════════════════════════════════════════════

TEST_CASE("StackStatus: Default initialization", "[telemetry][api]") {
    StackStatus s{};

    CHECK(s.uptime_seconds == 0);
    CHECK(s.traffic.packets_rx == 0);
    CHECK(s.traffic.bytes_rx == 0);
    CHECK_THAT(s.traffic.pps_rx, WithinAbs(0.0, 1e-9));
    CHECK(s.tcp.active_connections == 0);
    CHECK(s.tcp.rtt.samples == 0);
    CHECK_THAT(s.tcp.rtt.avg_us, WithinAbs(0.0, 1e-9));
    CHECK(s.security.firewall_enabled == false);
    CHECK(s.ai.enabled == false);
}

// ════════════════════════════════════════════
// Prometheus exporter 补充测试
// ════════════════════════════════════════════

TEST_CASE("PrometheusExporter: Histogram bucket order via TelemetryAPI", "[telemetry][api]") {
    TestEnv env;

    auto& h = env.registry.histogram("api_latency", "Latency",
                                      {0.5, 1.0, 2.5, 5.0, 10.0});
    h.observe(0.3);
    h.observe(1.5);
    h.observe(9.0);

    auto prom = env.api->to_prometheus();

    // 在 api_latency 这个指标块内查找桶顺序
    auto block_start = prom.find("api_latency_bucket");
    REQUIRE(block_start != std::string::npos);

    auto pos05 = prom.find("api_latency_bucket{le=\"0.5\"}", block_start);
    auto pos1 = prom.find("api_latency_bucket{le=\"1\"}", block_start);
    auto pos25 = prom.find("api_latency_bucket{le=\"2.5\"}", block_start);
    auto posInf = prom.find("api_latency_bucket{le=\"+Inf\"}", block_start);

    REQUIRE(pos05 != std::string::npos);
    REQUIRE(pos1 != std::string::npos);
    REQUIRE(pos25 != std::string::npos);
    REQUIRE(posInf != std::string::npos);

    CHECK(pos05 < pos1);
    CHECK(pos1 < pos25);
    CHECK(pos25 < posInf);
}
