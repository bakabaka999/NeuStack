#include "neustack/telemetry/metric_types.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <cinttypes>

using namespace neustack::telemetry;

// 简易 benchmark 框架 (与现有 bench_*.cpp 风格一致)

struct BenchResult {
    const char* name;
    double ns_per_op;
    uint64_t ops;
};

template<typename Fn>
BenchResult run_bench(const char* name, uint64_t ops, Fn fn) {
    auto start = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < ops; ++i) {
        fn();
    }
    auto end = std::chrono::high_resolution_clock::now();

    double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
    return {name, elapsed_ns / ops, ops};
}

void print_result(const BenchResult& r) {
    printf("  %-45s  %8.1f ns/op  (%" PRIu64 " ops)\n", r.name, r.ns_per_op, r.ops);
}

int main() {
    printf("=== Metrics Hotpath Benchmark ===\n\n");

    constexpr uint64_t OPS = 10000000;

    // Counter increment
    {
        Counter c(MetricMeta{"bench_counter", "", ""});
        auto r = run_bench("Counter::increment()", OPS, [&c]() {
            c.increment();
        });
        print_result(r);
    }

    // Counter increment with delta
    {
        Counter c(MetricMeta{"bench_counter", "", ""});
        auto r = run_bench("Counter::increment(100)", OPS, [&c]() {
            c.increment(100);
        });
        print_result(r);
    }

    // Gauge set
    {
        Gauge g(MetricMeta{"bench_gauge", "", ""});
        double val = 0;
        auto r = run_bench("Gauge::set(double)", OPS, [&g, &val]() {
            g.set(val);
            val += 0.1;
        });
        print_result(r);
    }

    // Gauge increment
    {
        Gauge g(MetricMeta{"bench_gauge", "", ""});
        auto r = run_bench("Gauge::increment(1.0)", OPS, [&g]() {
            g.increment(1.0);
        });
        print_result(r);
    }

    // Histogram observe - 5 buckets
    {
        Histogram h(MetricMeta{"bench_hist5", "", ""}, {100, 500, 1000, 5000, 10000});
        double val = 0;
        auto r = run_bench("Histogram::observe() [5 buckets]", OPS, [&h, &val]() {
            h.observe(val);
            val += 1.0;
            if (val > 20000) val = 0;
        });
        print_result(r);
    }

    // Histogram observe - 11 buckets
    {
        Histogram h(MetricMeta{"bench_hist11", "", ""},
                    {50, 100, 200, 500, 1000, 2000, 5000, 10000, 50000, 100000, 500000});
        double val = 0;
        auto r = run_bench("Histogram::observe() [11 buckets]", OPS, [&h, &val]() {
            h.observe(val);
            val += 1.0;
            if (val > 1000000) val = 0;
        });
        print_result(r);
    }

    // Histogram snapshot
    {
        Histogram h(MetricMeta{"bench_hist", "", ""}, {100, 500, 1000, 5000, 10000});
        for (int i = 0; i < 1000; ++i) h.observe(i * 10.0);
        auto r = run_bench("Histogram::snapshot()", OPS / 100, [&h]() {
            auto snap = h.snapshot();
            (void)snap;
        });
        print_result(r);
    }

    // Bridge gauge callback
    {
        uint64_t external = 0;
        auto cb = [&external]() { return static_cast<double>(external); };
        auto r = run_bench("BridgeGauge callback()", OPS, [&cb, &external]() {
            auto v = cb();
            (void)v;
            external++;
        });
        print_result(r);
    }

    // Registry find_counter lookup
    {
        MetricsRegistry reg;
        reg.counter("bench_lookup_target", "target");
        for (int i = 0; i < 49; ++i) {
            reg.counter("bench_other_" + std::to_string(i), "other");
        }
        auto r = run_bench("Registry::find_counter() [50 entries]", OPS / 10, [&reg]() {
            auto* c = reg.find_counter("bench_lookup_target");
            (void)c;
        });
        print_result(r);
    }

    // Contended counter (4 threads)
    printf("\n--- Multi-threaded ---\n");
    {
        Counter c(MetricMeta{"bench_contended", "", ""});
        constexpr int THREADS = 4;
        constexpr uint64_t OPS_PER_THREAD = OPS / THREADS;

        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&c]() {
                for (uint64_t i = 0; i < OPS_PER_THREAD; ++i) {
                    c.increment();
                }
            });
        }
        for (auto& th : threads) th.join();
        auto end = std::chrono::high_resolution_clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        printf("  %-45s  %8.1f ns/op  (%d threads x %" PRIu64 " ops)\n",
               "Counter::increment() [4 threads contended]",
               ns / (THREADS * OPS_PER_THREAD), THREADS, OPS_PER_THREAD);
    }

    printf("\n=== Done ===\n");
    return 0;
}