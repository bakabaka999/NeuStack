#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "neustack/telemetry/prometheus_exporter.hpp"
#include "neustack/telemetry/metrics_registry.hpp"

using namespace neustack::telemetry;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("PrometheusExporter: Empty registry", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    CHECK(output.empty());
}

TEST_CASE("PrometheusExporter: Counter format", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    auto& c = reg.counter("test_rx_total", "Total RX");
    c.increment(9999);

    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    
    CHECK_THAT(output, ContainsSubstring("# HELP test_rx_total Total RX"));
    CHECK_THAT(output, ContainsSubstring("# TYPE test_rx_total counter"));
    CHECK_THAT(output, ContainsSubstring("test_rx_total 9999"));
}

TEST_CASE("PrometheusExporter: Gauge format", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    auto& g = reg.gauge("test_temp", "Temperature");
    g.set(36.6);

    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    
    CHECK_THAT(output, ContainsSubstring("# TYPE test_temp gauge"));
    CHECK_THAT(output, ContainsSubstring("test_temp 36.6"));
}

TEST_CASE("PrometheusExporter: Histogram format", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    auto& h = reg.histogram("test_rtt_us", "RTT", {100, 500, 1000});
    h.observe(50);
    h.observe(200);

    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    
    CHECK_THAT(output, ContainsSubstring("# TYPE test_rtt_us histogram"));
    CHECK_THAT(output, ContainsSubstring("test_rtt_us_bucket{le=\"100\"}"));
    CHECK_THAT(output, ContainsSubstring("test_rtt_us_bucket{le=\"+Inf\"}"));
    CHECK_THAT(output, ContainsSubstring("test_rtt_us_sum"));
    CHECK_THAT(output, ContainsSubstring("test_rtt_us_count 2"));
}

TEST_CASE("PrometheusExporter: Bridge gauge as gauge type", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    reg.bridge_gauge("test_bridge", "Bridge metric", []() { return 42.0; });

    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    
    CHECK_THAT(output, ContainsSubstring("# TYPE test_bridge gauge"));
    CHECK_THAT(output, ContainsSubstring("test_bridge 42"));
}

TEST_CASE("PrometheusExporter: Integer gauge no decimals", "[telemetry][prometheus]") {
    MetricsRegistry reg;
    auto& g = reg.gauge("test_conns", "Connections");
    g.set(42.0);

    PrometheusExporter exporter;
    auto output = exporter.serialize(reg);
    
    // 可以输出 42 或 42.0，都是合法的，这里我们的实现倾向于去尾零
    CHECK_THAT(output, ContainsSubstring("test_conns 42"));
}
