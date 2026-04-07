#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "neustack/telemetry/telemetry_api.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "neustack/common/json_builder.hpp"
#include "../../helpers/json_validator.hpp"

// http_endpoints 的公开 API 只有 register_http_endpoints，
// 内部 serialize_*_json 是 static 函数无法直接测。
// 本文件通过 TelemetryAPI 子接口验证各端点的数据来源正确性，
// 同时直接测试 JsonBuilder 产出的 JSON 结构。

using namespace neustack::telemetry;
using neustack::JsonBuilder;
using neustack::test::JsonValidator;
using Catch::Matchers::ContainsSubstring;

// ════════════════════════════════════════════
// Fixture
// ════════════════════════════════════════════

struct EndpointFixture {
    neustack::GlobalMetrics         gm;
    neustack::telemetry::MetricsRegistry registry;
    neustack::TCPConnectionManager  tcp_mgr{0};  // 0 = 无 IP，不监听

    TelemetryAPI api;

    EndpointFixture()
        : api(gm, nullptr, tcp_mgr, nullptr, nullptr, registry,
              std::chrono::steady_clock::now())
    {}
};

// ════════════════════════════════════════════
// traffic() → serialize_traffic_json 的数据来源
// ════════════════════════════════════════════

TEST_CASE("endpoint data: traffic counters reflected in API", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    fix.gm.packets_rx.store(42,     std::memory_order_relaxed);
    fix.gm.packets_tx.store(10,     std::memory_order_relaxed);
    fix.gm.bytes_rx.store(1024,     std::memory_order_relaxed);
    fix.gm.bytes_tx.store(2048,     std::memory_order_relaxed);

    auto t = fix.api.traffic();
    CHECK(t.packets_rx == 42);
    CHECK(t.packets_tx == 10);
    CHECK(t.bytes_rx   == 1024);
    CHECK(t.bytes_tx   == 2048);
}

TEST_CASE("endpoint data: traffic JSON structure is valid", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    fix.gm.packets_rx.store(100, std::memory_order_relaxed);
    fix.gm.bytes_tx.store(512,   std::memory_order_relaxed);

    // 直接用 JsonBuilder 模拟 serialize_traffic_json 逻辑验证结构
    auto t = fix.api.traffic();
    JsonBuilder b(false, 256);
    b.begin_object();
    b.key("packets_rx"); b.write_uint64(t.packets_rx); b.comma();
    b.key("packets_tx"); b.write_uint64(t.packets_tx); b.comma();
    b.key("bytes_rx");   b.write_uint64(t.bytes_rx);   b.comma();
    b.key("bytes_tx");   b.write_uint64(t.bytes_tx);   b.comma();
    b.key("pps_rx");     b.write_double(t.pps_rx);     b.comma();
    b.key("pps_tx");     b.write_double(t.pps_tx);     b.comma();
    b.key("bps_rx");     b.write_double(t.bps_rx);     b.comma();
    b.key("bps_tx");     b.write_double(t.bps_tx);
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"packets_rx\""));
    CHECK_THAT(b.buf, ContainsSubstring("100"));
    CHECK_THAT(b.buf, ContainsSubstring("\"bytes_tx\""));
    CHECK_THAT(b.buf, ContainsSubstring("512"));
}

// ════════════════════════════════════════════
// tcp_stats() → serialize_tcp_json 的数据来源
// ════════════════════════════════════════════

TEST_CASE("endpoint data: tcp stats JSON structure is valid", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    fix.gm.active_connections.store(5,  std::memory_order_relaxed);
    fix.gm.conn_established.store(100,  std::memory_order_relaxed);
    fix.gm.total_retransmits.store(7,   std::memory_order_relaxed);

    auto t = fix.api.tcp_stats();
    CHECK(t.active_connections == 5);
    CHECK(t.total_established  == 100);
    CHECK(t.total_retransmits  == 7);

    JsonBuilder b(false, 512);
    b.begin_object();
    b.key("active_connections"); b.write_uint64(t.active_connections); b.comma();
    b.key("total_established");  b.write_uint64(t.total_established);  b.comma();
    b.key("total_reset");        b.write_uint64(t.total_reset);        b.comma();
    b.key("total_timeout");      b.write_uint64(t.total_timeout);      b.comma();
    b.key("total_retransmits");  b.write_uint64(t.total_retransmits);  b.comma();
    b.key("avg_cwnd");           b.write_double(t.avg_cwnd);           b.comma();
    b.key("rtt");
    b.begin_object();
    b.key("min_us");  b.write_double(t.rtt.min_us);  b.comma();
    b.key("avg_us");  b.write_double(t.rtt.avg_us);  b.comma();
    b.key("p50_us");  b.write_double(t.rtt.p50_us);  b.comma();
    b.key("p90_us");  b.write_double(t.rtt.p90_us);  b.comma();
    b.key("p99_us");  b.write_double(t.rtt.p99_us);  b.comma();
    b.key("max_us");  b.write_double(t.rtt.max_us);  b.comma();
    b.key("samples"); b.write_uint64(t.rtt.samples);
    b.end_object();
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"active_connections\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"rtt\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"p99_us\""));
}

TEST_CASE("endpoint data: rtt populated from histogram", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto& h = fix.registry.histogram("neustack_tcp_rtt_us", "RTT",
                                     {50, 100, 500, 1000, 5000});
    for (int i = 0; i < 100; ++i) h.observe(80.0);   // 全部落在 (50,100] 桶
    for (int i = 0; i < 10;  ++i) h.observe(4000.0); // 落在 (1000,5000] 桶

    auto t = fix.api.tcp_stats();
    CHECK(t.rtt.samples == 110);
    CHECK(t.rtt.p50_us  > 0.0);
    CHECK(t.rtt.p99_us  > t.rtt.p50_us);  // P99 应大于 P50
}

// ════════════════════════════════════════════
// security_stats() → serialize_security_json 的数据来源
// ════════════════════════════════════════════

TEST_CASE("endpoint data: security defaults when firewall/ai disabled", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto s = fix.api.security_stats();

    CHECK(s.firewall_enabled == false);
    CHECK(s.ai_enabled       == false);
    CHECK(s.agent_state      == "DISABLED");
    CHECK(s.anomaly_score    == 0.0f);
}

TEST_CASE("endpoint data: security JSON structure is valid", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto s = fix.api.security_stats();

    JsonBuilder b(false, 512);
    b.begin_object();
    b.key("firewall_enabled");        b.write_bool(s.firewall_enabled);                                b.comma();
    b.key("shadow_mode");             b.write_bool(s.shadow_mode);                                    b.comma();
    b.key("ai_enabled");              b.write_bool(s.ai_enabled);                                     b.comma();
    b.key("pps");                     b.write_double(s.pps);                                          b.comma();
    b.key("syn_rate");                b.write_double(s.syn_rate);                                     b.comma();
    b.key("syn_synack_ratio");        b.write_double(s.syn_synack_ratio);                             b.comma();
    b.key("rst_ratio");               b.write_double(s.rst_ratio);                                    b.comma();
    b.key("packets_dropped");         b.write_uint64(s.packets_dropped);                              b.comma();
    b.key("packets_alerted");         b.write_uint64(s.packets_alerted);                              b.comma();
    b.key("anomaly_score");           b.write_double(static_cast<double>(s.anomaly_score));           b.comma();
    b.key("agent_state");             b.write_string(s.agent_state);                                  b.comma();
    b.key("predicted_bandwidth_bps"); b.write_double(static_cast<double>(s.predicted_bandwidth_bps));
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("false"));          // firewall_enabled
    CHECK_THAT(b.buf, ContainsSubstring("\"DISABLED\""));   // agent_state
}

TEST_CASE("endpoint data: health JSON includes uptime", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto status = fix.api.status();

    JsonBuilder b(false, 128);
    b.begin_object();
    b.key("status");         b.write_string("ok");            b.comma();
    b.key("version");        b.write_string("1.5.0");        b.comma();
    b.key("uptime_seconds"); b.write_uint64(status.uptime_seconds);
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"status\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"uptime_seconds\""));
}

// ════════════════════════════════════════════
// connections() → serialize_connections_json 的数据来源
// ════════════════════════════════════════════

TEST_CASE("endpoint data: empty connections list JSON is valid", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto conns = fix.api.connections();
    CHECK(conns.empty());

    JsonBuilder b(false, 128);
    b.begin_object();
    b.key("count");       b.write_uint64(conns.size()); b.comma();
    b.key("connections"); b.begin_array(); b.end_array();
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"count\""));
    CHECK_THAT(b.buf, ContainsSubstring("0"));
    CHECK_THAT(b.buf, ContainsSubstring("[]"));
}

// ════════════════════════════════════════════
// error_response JSON 结构
// ════════════════════════════════════════════

TEST_CASE("endpoint data: error response JSON is valid", "[http_endpoints][telemetry]") {
    // 模拟 error_response 的 JsonBuilder 逻辑
    JsonBuilder b(false);
    b.begin_object();
    b.key("error");   b.write_string("Not Found");    b.comma();
    b.key("message"); b.write_string("Unknown endpoint."); b.comma();
    b.key("status");  b.write_uint64(404);
    b.end_object();

    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"error\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"Not Found\""));
    CHECK_THAT(b.buf, ContainsSubstring("404"));
}

// ════════════════════════════════════════════
// pretty vs compact
// ════════════════════════════════════════════

TEST_CASE("endpoint data: pretty output larger than compact", "[http_endpoints][telemetry]") {
    EndpointFixture fix;
    auto t = fix.api.traffic();

    auto make = [&](bool pretty) {
        JsonBuilder b(pretty, 256);
        b.begin_object();
        b.key("packets_rx"); b.write_uint64(t.packets_rx); b.comma();
        b.key("bytes_rx");   b.write_uint64(t.bytes_rx);
        b.end_object();
        return b.buf;
    };

    auto compact = make(false);
    auto pretty  = make(true);

    CHECK(JsonValidator::is_valid(compact));
    CHECK(JsonValidator::is_valid(pretty));
    CHECK(pretty.size() > compact.size());
    CHECK_THAT(pretty, ContainsSubstring("\n"));
}
