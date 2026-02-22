#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "neustack/telemetry/json_exporter.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include "../../helpers/json_validator.hpp"

using namespace neustack::telemetry;
using namespace neustack::test;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("JsonExporter: Empty registry", "[telemetry][json]") {
    MetricsRegistry reg;
    JsonExporter exporter;
    auto json = exporter.serialize(reg);

    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"timestamp\""));
}

TEST_CASE("JsonExporter: Single counter", "[telemetry][json]") {
    MetricsRegistry reg;
    auto& c = reg.counter("test_rx", "RX packets", "packets");
    c.increment(12345);

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"test_rx\""));
    CHECK_THAT(json, ContainsSubstring("12345"));
}

TEST_CASE("JsonExporter: Single gauge", "[telemetry][json]") {
    MetricsRegistry reg;
    auto& g = reg.gauge("test_temp", "Temperature");
    g.set(36.6);

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"test_temp\""));
    CHECK_THAT(json, ContainsSubstring("36.6"));
}

TEST_CASE("JsonExporter: Histogram with buckets", "[telemetry][json]") {
    MetricsRegistry reg;
    auto& h = reg.histogram("test_rtt", "RTT", {100, 500, 1000});
    h.observe(50);
    h.observe(200);
    h.observe(800);

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"test_rtt\""));
    CHECK_THAT(json, ContainsSubstring("\"count\""));
    CHECK_THAT(json, ContainsSubstring("\"sum\""));
    CHECK_THAT(json, ContainsSubstring("\"buckets\""));
}

TEST_CASE("JsonExporter: Bridge gauge calls callback", "[telemetry][json]") {
    MetricsRegistry reg;
    reg.bridge_gauge("test_bridge", "Bridge", []() { return 42.0; });

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"test_bridge\""));
    CHECK_THAT(json, ContainsSubstring("42"));
}

TEST_CASE("JsonExporter: Pretty format has newlines", "[telemetry][json]") {
    MetricsRegistry reg;
    reg.counter("test_c", "Test");

    JsonExporter compact(false);
    JsonExporter pretty(true);

    auto compact_json = compact.serialize(reg);
    auto pretty_json = pretty.serialize(reg);

    // pretty 版本应该更长（含缩进和换行）
    CHECK(pretty_json.size() > compact_json.size());
    CHECK_THAT(pretty_json, ContainsSubstring("\n"));
}

TEST_CASE("JsonExporter: Filter by prefix", "[telemetry][json]") {
    MetricsRegistry reg;
    reg.counter("neustack_tcp_rx", "TCP RX");
    reg.counter("neustack_tcp_tx", "TCP TX");
    reg.counter("neustack_udp_rx", "UDP RX");

    JsonExporter exporter;
    auto json = exporter.serialize_filtered(reg, "neustack_tcp");
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("neustack_tcp_rx"));
    CHECK_THAT(json, ContainsSubstring("neustack_tcp_tx"));
    // UDP 不应出现
    CHECK_THAT(json, !ContainsSubstring("neustack_udp_rx"));
}

TEST_CASE("JsonExporter: Zero value gauge included", "[telemetry][json]") {
    MetricsRegistry reg;
    reg.gauge("test_zero", "Zero gauge");
    // 不设置值，默认为 0

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK_THAT(json, ContainsSubstring("\"test_zero\""));
}

TEST_CASE("JsonExporter: Multiple metric types", "[telemetry][json]") {
    MetricsRegistry reg;
    reg.counter("c1", "Counter 1");
    reg.gauge("g1", "Gauge 1");
    reg.histogram("h1", "Histogram 1", {100});
    reg.bridge_gauge("b1", "Bridge 1", []() { return 1.0; });

    JsonExporter exporter;
    auto json = exporter.serialize(reg);
    CHECK(JsonValidator::is_valid(json));
    CHECK_THAT(json, ContainsSubstring("\"c1\""));
    CHECK_THAT(json, ContainsSubstring("\"g1\""));
    CHECK_THAT(json, ContainsSubstring("\"h1\""));
    CHECK_THAT(json, ContainsSubstring("\"b1\""));
}
