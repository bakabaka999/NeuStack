#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "neustack/telemetry/metric_types.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include <thread>
#include <vector>
#include <cmath>

using namespace neustack::telemetry;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Counter 测试
// ============================================================================

TEST_CASE("Counter: Initial value is zero", "[telemetry][counter]") {
    Counter c(MetricMeta{"test_counter", "A test counter", ""});
    CHECK(c.value() == 0);
}

TEST_CASE("Counter: Single increment", "[telemetry][counter]") {
    Counter c(MetricMeta{"test_counter", "A test counter", ""});
    c.increment();
    CHECK(c.value() == 1);
}

TEST_CASE("Counter: Increment with delta", "[telemetry][counter]") {
    Counter c(MetricMeta{"test_counter", "A test counter", ""});
    c.increment(100);
    CHECK(c.value() == 100);
}

TEST_CASE("Counter: Multiple increments accumulate", "[telemetry][counter]") {
    Counter c(MetricMeta{"test_counter", "A test counter", ""});
    c.increment(10);
    c.increment(20);
    c.increment(30);
    CHECK(c.value() == 60);
}

TEST_CASE("Counter: Meta information preserved", "[telemetry][counter]") {
    Counter c(MetricMeta{"my_counter", "Help text", "bytes"});
    CHECK(c.meta().name == "my_counter");
    CHECK(c.meta().help == "Help text");
    CHECK(c.meta().unit == "bytes");
}

TEST_CASE("Counter: Concurrent increments", "[telemetry][counter][threads]") {
    Counter c(MetricMeta{"test_counter", "Concurrent test", ""});

    constexpr int NUM_THREADS = 4;
    constexpr int INCREMENTS_PER_THREAD = 100000;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&c]() {
            for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
                c.increment();
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    CHECK(c.value() == NUM_THREADS * INCREMENTS_PER_THREAD);
}

// ============================================================================
// Gauge 测试
// ============================================================================

TEST_CASE("Gauge: Initial value is zero", "[telemetry][gauge]") {
    Gauge g(MetricMeta{"test_gauge", "A test gauge", ""});
    CHECK_THAT(g.value(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Gauge: Set value", "[telemetry][gauge]") {
    Gauge g(MetricMeta{"test_gauge", "A test gauge", ""});
    g.set(42.5);
    CHECK_THAT(g.value(), WithinAbs(42.5, 1e-9));
}

TEST_CASE("Gauge: Increment and decrement", "[telemetry][gauge]") {
    Gauge g(MetricMeta{"test_gauge", "A test gauge", ""});
    g.set(10.0);
    g.increment(5.0);
    CHECK_THAT(g.value(), WithinAbs(15.0, 1e-9));
    g.decrement(3.0);
    CHECK_THAT(g.value(), WithinAbs(12.0, 1e-9));
}

TEST_CASE("Gauge: Negative value", "[telemetry][gauge]") {
    Gauge g(MetricMeta{"test_gauge", "A test gauge", ""});
    g.set(-1.0);
    CHECK_THAT(g.value(), WithinAbs(-1.0, 1e-9));
}

TEST_CASE("Gauge: Special float values", "[telemetry][gauge]") {
    Gauge g(MetricMeta{"test_gauge", "A test gauge", ""});

    SECTION("Zero") {
        g.set(0.0);
        CHECK_THAT(g.value(), WithinAbs(0.0, 1e-9));
    }

    SECTION("Infinity") {
        g.set(std::numeric_limits<double>::infinity());
        CHECK(std::isinf(g.value()));
    }

    SECTION("NaN") {
        g.set(std::numeric_limits<double>::quiet_NaN());
        CHECK(std::isnan(g.value()));
    }
}

// ============================================================================
// Histogram 测试
// ============================================================================

TEST_CASE("Histogram: Single observation", "[telemetry][histogram]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {100, 500, 1000});
    h.observe(50.0);  // 落入 [0, 100] 桶

    auto snap = h.snapshot();
    CHECK(snap.count == 1);
    CHECK_THAT(snap.sum, WithinAbs(50.0, 1e-9));
    CHECK(snap.bucket_counts[0] == 1);  // le=100
    CHECK(snap.bucket_counts[1] == 1);  // le=500 (累积)
    CHECK(snap.bucket_counts[2] == 1);  // le=1000 (累积)
    CHECK(snap.bucket_counts[3] == 1);  // le=+Inf (累积)
}

TEST_CASE("Histogram: Multiple observations across buckets", "[telemetry][histogram]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {100, 500, 1000});

    h.observe(50.0);   // 桶 0 (le=100)
    h.observe(200.0);  // 桶 1 (le=500)
    h.observe(800.0);  // 桶 2 (le=1000)
    h.observe(5000.0); // 桶 3 (+Inf)

    auto snap = h.snapshot();
    CHECK(snap.count == 4);
    CHECK_THAT(snap.sum, WithinAbs(6050.0, 1e-9));

    // 累积桶
    CHECK(snap.bucket_counts[0] == 1);  // le=100:  50
    CHECK(snap.bucket_counts[1] == 2);  // le=500:  50, 200
    CHECK(snap.bucket_counts[2] == 3);  // le=1000: 50, 200, 800
    CHECK(snap.bucket_counts[3] == 4);  // +Inf:    全部
}

TEST_CASE("Histogram: Cumulative buckets are non-decreasing", "[telemetry][histogram]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {10, 50, 100, 500, 1000});

    for (int i = 1; i <= 100; ++i) {
        h.observe(static_cast<double>(i * 10));
    }

    auto snap = h.snapshot();
    for (size_t i = 1; i < snap.bucket_counts.size(); ++i) {
        CHECK(snap.bucket_counts[i] >= snap.bucket_counts[i - 1]);
    }
}

TEST_CASE("Histogram: Empty snapshot", "[telemetry][histogram]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {100, 500});

    auto snap = h.snapshot();
    CHECK(snap.count == 0);
    CHECK_THAT(snap.sum, WithinAbs(0.0, 1e-9));
    for (auto cnt : snap.bucket_counts) {
        CHECK(cnt == 0);
    }
}

TEST_CASE("Histogram: Value exactly on boundary", "[telemetry][histogram]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {100, 500});

    h.observe(100.0);  // 恰好在边界，应落入 le=100 桶
    auto snap = h.snapshot();
    CHECK(snap.bucket_counts[0] == 1);
}

TEST_CASE("Histogram: Concurrent observations", "[telemetry][histogram][threads]") {
    Histogram h(MetricMeta{"test_hist", "Test", ""}, {100, 500, 1000});

    constexpr int NUM_THREADS = 4;
    constexpr int OBS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&h]() {
            for (int i = 0; i < OBS_PER_THREAD; ++i) {
                h.observe(50.0);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto snap = h.snapshot();
    CHECK(snap.count == NUM_THREADS * OBS_PER_THREAD);
}

// ============================================================================
// MetricsRegistry 测试
// ============================================================================

TEST_CASE("Registry: Register and find counter", "[telemetry][registry]") {
    MetricsRegistry reg;
    auto& c = reg.counter("test_rx", "RX packets");
    c.increment(42);

    auto* found = reg.find_counter("test_rx");
    REQUIRE(found != nullptr);
    CHECK(found->value() == 42);
}

TEST_CASE("Registry: Register and find gauge", "[telemetry][registry]") {
    MetricsRegistry reg;
    auto& g = reg.gauge("test_temp", "Temperature");
    g.set(36.6);

    auto* found = reg.find_gauge("test_temp");
    REQUIRE(found != nullptr);
    CHECK_THAT(found->value(), WithinAbs(36.6, 1e-9));
}

TEST_CASE("Registry: Register and find histogram", "[telemetry][registry]") {
    MetricsRegistry reg;
    auto& h = reg.histogram("test_rtt", "RTT", {100, 500});
    h.observe(250);

    auto* found = reg.find_histogram("test_rtt");
    REQUIRE(found != nullptr);
    CHECK(found->snapshot().count == 1);
}

TEST_CASE("Registry: Bridge gauge callback", "[telemetry][registry]") {
    MetricsRegistry reg;
    double external_value = 42.0;

    reg.bridge_gauge("test_bridge", "Bridge test",
                     [&external_value]() { return external_value; });

    auto* bridge = reg.find_bridge("test_bridge");
    REQUIRE(bridge != nullptr);
    CHECK_THAT(bridge->callback(), WithinAbs(42.0, 1e-9));

    external_value = 99.0;
    CHECK_THAT(bridge->callback(), WithinAbs(99.0, 1e-9));
}

TEST_CASE("Registry: Find non-existent returns nullptr", "[telemetry][registry]") {
    MetricsRegistry reg;
    CHECK(reg.find_counter("no_such_counter") == nullptr);
    CHECK(reg.find_gauge("no_such_gauge") == nullptr);
    CHECK(reg.find_histogram("no_such_histogram") == nullptr);
    CHECK(reg.find_bridge("no_such_bridge") == nullptr);
}

TEST_CASE("Registry: Entry order preserved", "[telemetry][registry]") {
    MetricsRegistry reg;
    reg.counter("aaa", "First");
    reg.gauge("bbb", "Second");
    reg.histogram("ccc", "Third", {100});
    reg.bridge_gauge("ddd", "Fourth", []() { return 0.0; });

    auto& entries = reg.entries();
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].name == "aaa");
    CHECK(entries[0].kind == MetricsRegistry::MetricKind::COUNTER);
    CHECK(entries[1].name == "bbb");
    CHECK(entries[1].kind == MetricsRegistry::MetricKind::GAUGE);
    CHECK(entries[2].name == "ccc");
    CHECK(entries[2].kind == MetricsRegistry::MetricKind::HISTOGRAM);
    CHECK(entries[3].name == "ddd");
    CHECK(entries[3].kind == MetricsRegistry::MetricKind::BRIDGE_GAUGE);
}