/**
 * AF_XDP Datapath Micro-Benchmark
 *
 * 量化 AF_XDP 数据面各组件的性能：
 *   Test 1: UMEM Frame Alloc/Free
 *   Test 2: XDP Ring Operations
 *   Test 3: Zero-Copy vs Traditional Send Path
 *   Test 4: Prefetch Effect
 *   Test 5: build_header_only vs build
 *   Test 6: GlobalMetrics Hot Path
 *
 * Test 1-2 需要 Linux 平台 (UMEM/XdpRing 仅 Linux 可用)。
 * Test 3-6 跨平台可跑。
 *
 * Usage:
 *   ./bench_afxdp_datapath           # human-readable output
 *   ./bench_afxdp_datapath --json    # structured JSON output
 */

#include "neustack/transport/tcp_builder.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <ctime>
#include <vector>
#include <memory>
#include <cstdlib>
#include <string>
#include <map>

#ifdef NEUSTACK_PLATFORM_LINUX
#include "neustack/hal/umem.hpp"
#include "neustack/hal/xdp_ring.hpp"
#endif

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

// Global flag: JSON output mode
static bool json_mode = false;

// ─────────────────────────────────────────────────────────
// Result collection for JSON mode
// ─────────────────────────────────────────────────────────

struct MetricValue {
    double value;
};

// Flat key-value store: "section.subsection.metric" → value
static std::map<std::string, double> g_results;

static void record(const std::string& key, double value) {
    g_results[key] = value;
}

// ─────────────────────────────────────────────────────────
// JSON output helper (hand-rolled, no third-party deps)
// ─────────────────────────────────────────────────────────

static std::string get_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

static void print_json_results() {
    printf("{\n");
    printf("  \"benchmark\": \"afxdp_datapath\",\n");
    printf("  \"timestamp\": \"%s\",\n", get_iso_timestamp().c_str());
    printf("  \"results\": {\n");

    // Group by first key component
    std::map<std::string, std::map<std::string, double>> sections;
    for (const auto& [key, val] : g_results) {
        auto dot = key.find('.');
        if (dot != std::string::npos) {
            std::string section = key.substr(0, dot);
            std::string rest = key.substr(dot + 1);
            sections[section][rest] = val;
        }
    }

    size_t sec_idx = 0;
    for (const auto& [section, metrics] : sections) {
        printf("    \"%s\": {\n", section.c_str());
        size_t m_idx = 0;
        for (const auto& [metric, val] : metrics) {
            printf("      \"%s\": %.6g", metric.c_str(), val);
            if (++m_idx < metrics.size()) printf(",");
            printf("\n");
        }
        printf("    }");
        if (++sec_idx < sections.size()) printf(",");
        printf("\n");
    }

    printf("  }\n");
    printf("}\n");
}

// 防止编译器优化
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

// ─────────────────────────────────────────────────────────
// Test 1: UMEM Frame Alloc/Free
// ─────────────────────────────────────────────────────────
#ifdef NEUSTACK_PLATFORM_LINUX

static void bench_umem_alloc_free() {
    if (!json_mode) printf("[UMEM Frame Alloc/Free]\n");

    constexpr uint32_t FRAME_COUNT = 4096;
    constexpr uint32_t FRAME_SIZE  = 4096;

    // --- Sequential alloc+free ---
    {
        UmemFrameAllocator allocator(FRAME_COUNT, FRAME_SIZE);
        constexpr uint64_t ITERS = 2000000;

        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            uint64_t addr = allocator.alloc();
            do_not_optimize(addr);
            allocator.free(addr);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;
        double mops = (ITERS / (ns / 1e9)) / 1e6;

        record("umem_alloc_free.sequential_ns_per_op", ns_per_op);
        record("umem_alloc_free.sequential_mops", mops);

        if (!json_mode)
            printf("  sequential alloc+free:          %6.1f ns/op  (%5.1f M ops/s)\n",
                   ns_per_op, mops);
    }

    // --- Batch drain-all + free-all ---
    {
        UmemFrameAllocator allocator(FRAME_COUNT, FRAME_SIZE);
        constexpr uint32_t ROUNDS = 1000;
        uint64_t addrs[FRAME_COUNT];

        auto start = Clock::now();
        for (uint32_t r = 0; r < ROUNDS; ++r) {
            uint32_t n = allocator.alloc_batch(addrs, FRAME_COUNT);
            do_not_optimize(n);
            allocator.free_batch(addrs, n);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        uint64_t total_ops = static_cast<uint64_t>(ROUNDS) * FRAME_COUNT;
        double ns_per_op = ns / total_ops;
        double mops = (total_ops / (ns / 1e9)) / 1e6;

        record("umem_alloc_free.batch_ns_per_op", ns_per_op);
        record("umem_alloc_free.batch_mops", mops);
        record("umem_alloc_free.batch_frame_count", FRAME_COUNT);

        if (!json_mode)
            printf("  batch drain+free (%u frames):  %6.1f ns/op  (%5.1f M ops/s)\n",
                   FRAME_COUNT, ns_per_op, mops);
    }

    if (!json_mode) printf("\n");
}

// ─────────────────────────────────────────────────────────
// Test 2: XDP Ring Operations
// ─────────────────────────────────────────────────────────

static void bench_xdp_ring() {
    if (!json_mode) printf("[XDP Ring Operations]\n");

    constexpr uint32_t RING_SIZE = 256; // power of 2
    constexpr uint64_t TOTAL_OPS = 1000000;
    constexpr uint32_t BATCH_SIZES[] = {1, 8, 32, 64, 128};

    uint32_t producer = 0;
    uint32_t consumer = 0;
    uint64_t descs[RING_SIZE];

    for (uint32_t batch : BATCH_SIZES) {
        XdpRing<uint64_t> ring;
        producer = 0;
        consumer = 0;
        std::memset(descs, 0, sizeof(descs));
        ring.init(&producer, &consumer, descs, RING_SIZE);

        uint64_t iters = TOTAL_OPS / batch;

        auto start = Clock::now();
        for (uint64_t i = 0; i < iters; ++i) {
            uint32_t n = ring.reserve(batch);
            for (uint32_t j = 0; j < n; ++j) {
                ring.ring_at(j) = i * batch + j;
            }
            ring.submit(n);

            uint32_t m = ring.peek(batch);
            for (uint32_t j = 0; j < m; ++j) {
                do_not_optimize(ring.ring_at_consumer(j));
            }
            ring.release(m);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / (iters * batch);

        std::string key = "xdp_ring.batch_" + std::to_string(batch) + "_ns_per_op";
        record(key, ns_per_op);

        if (!json_mode)
            printf("  batch=%-3u:  %6.1f ns/op\n", batch, ns_per_op);
    }

    if (!json_mode) printf("\n");
}

#endif // NEUSTACK_PLATFORM_LINUX

// ─────────────────────────────────────────────────────────
// Test 3: Zero-Copy vs Traditional Send Path
// ─────────────────────────────────────────────────────────

static void bench_zero_copy() {
    if (!json_mode) printf("[Zero-Copy vs Traditional Send Path]\n");

    constexpr size_t PAYLOAD_SIZE = 1400;
    constexpr size_t ETH_HDR = 14;
    constexpr size_t IP_HDR  = 20;
    constexpr size_t TCP_HDR = 20;
    constexpr size_t TOTAL_HDR = ETH_HDR + IP_HDR + TCP_HDR; // 54
    constexpr uint64_t ITERS = 500000;

    uint8_t payload[PAYLOAD_SIZE];
    std::memset(payload, 0xAB, PAYLOAD_SIZE);

    uint8_t tcp_buf[PAYLOAD_SIZE + TCP_HDR];
    uint8_t ip_buf[PAYLOAD_SIZE + TCP_HDR + IP_HDR];
    uint8_t kern_buf[PAYLOAD_SIZE + TOTAL_HDR];
    uint8_t frame[PAYLOAD_SIZE + TOTAL_HDR];

    // --- Traditional: 3 copies ---
    double trad_ns_per_pkt;
    {
        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            std::memcpy(tcp_buf + TCP_HDR, payload, PAYLOAD_SIZE);
            std::memcpy(ip_buf + IP_HDR, tcp_buf, PAYLOAD_SIZE + TCP_HDR);
            std::memcpy(kern_buf + ETH_HDR, ip_buf, PAYLOAD_SIZE + TCP_HDR + IP_HDR);
            do_not_optimize(kern_buf[0]);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        trad_ns_per_pkt = ns / ITERS;

        record("zero_copy.traditional_ns_per_pkt", trad_ns_per_pkt);

        if (!json_mode)
            printf("  traditional (3 copies, %zuB):  %6.1f ns/pkt\n", PAYLOAD_SIZE, trad_ns_per_pkt);
    }

    // --- Zero-copy: 1 copy + header write ---
    double zc_ns_per_pkt;
    {
        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            std::memcpy(frame + TOTAL_HDR, payload, PAYLOAD_SIZE);
            std::memset(frame, 0, TOTAL_HDR);
            do_not_optimize(frame[0]);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        zc_ns_per_pkt = ns / ITERS;

        record("zero_copy.zero_copy_ns_per_pkt", zc_ns_per_pkt);

        if (!json_mode)
            printf("  zero-copy (1 copy, %zuB):      %6.1f ns/pkt\n", PAYLOAD_SIZE, zc_ns_per_pkt);
    }

    double speedup = trad_ns_per_pkt / zc_ns_per_pkt;
    record("zero_copy.speedup", speedup);

    if (!json_mode) {
        printf("  speedup:                        %.2fx\n", speedup);
        printf("\n");
    }
}

// ─────────────────────────────────────────────────────────
// Test 4: Prefetch Effect
// ─────────────────────────────────────────────────────────

static void bench_prefetch() {
    if (!json_mode) printf("[Prefetch Effect]\n");

    constexpr size_t BUF_SIZE = 1500;
    constexpr size_t NUM_BUFS = 4096;
    constexpr uint32_t ROUNDS = 1000;

    std::vector<std::unique_ptr<uint8_t[]>> bufs(NUM_BUFS);
    for (size_t i = 0; i < NUM_BUFS; ++i) {
        bufs[i] = std::make_unique<uint8_t[]>(BUF_SIZE);
        std::memset(bufs[i].get(), static_cast<int>(i & 0xFF), BUF_SIZE);
    }

    std::vector<uint8_t*> ptrs(NUM_BUFS);
    for (size_t i = 0; i < NUM_BUFS; ++i) {
        ptrs[i] = bufs[i].get();
    }

    // --- Without prefetch ---
    double no_pf_ns;
    {
        auto start = Clock::now();
        for (uint32_t r = 0; r < ROUNDS; ++r) {
            for (size_t i = 0; i < NUM_BUFS; ++i) {
                do_not_optimize(ptrs[i][0]);
                do_not_optimize(ptrs[i][BUF_SIZE - 1]);
            }
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        no_pf_ns = ns / (static_cast<uint64_t>(ROUNDS) * NUM_BUFS);

        record("prefetch.without_ns_per_pkt", no_pf_ns);

        if (!json_mode)
            printf("  without prefetch:  %6.1f ns/pkt\n", no_pf_ns);
    }

    // --- With prefetch ---
    double pf_ns;
    {
        auto start = Clock::now();
        for (uint32_t r = 0; r < ROUNDS; ++r) {
            for (size_t i = 0; i < NUM_BUFS; ++i) {
                if (i + 1 < NUM_BUFS) {
                    __builtin_prefetch(ptrs[i + 1], 0, 1);
                    __builtin_prefetch(ptrs[i + 1] + BUF_SIZE - 64, 0, 1);
                }
                do_not_optimize(ptrs[i][0]);
                do_not_optimize(ptrs[i][BUF_SIZE - 1]);
            }
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        pf_ns = ns / (static_cast<uint64_t>(ROUNDS) * NUM_BUFS);

        record("prefetch.with_ns_per_pkt", pf_ns);

        if (!json_mode)
            printf("  with prefetch:     %6.1f ns/pkt\n", pf_ns);
    }

    double improvement = (no_pf_ns - pf_ns) / no_pf_ns * 100.0;
    record("prefetch.improvement_pct", improvement);

    if (!json_mode) {
        printf("  improvement:       %5.1f%%\n", improvement);
        printf("\n");
    }
}

// ─────────────────────────────────────────────────────────
// Test 5: build_header_only vs build
// ─────────────────────────────────────────────────────────

static void bench_header_build() {
    if (!json_mode) printf("[Header Build: build() vs build_header_only()]\n");

    constexpr size_t PAYLOAD_SIZE = 1400;
    constexpr uint64_t ITERS = 1000000;

    uint8_t payload[PAYLOAD_SIZE];
    std::memset(payload, 0xAB, PAYLOAD_SIZE);
    uint8_t buffer[2048];

    // --- TCPBuilder::build (with payload copy) ---
    {
        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            TCPBuilder builder;
            builder.set_src_port(80)
                   .set_dst_port(12345)
                   .set_seq(static_cast<uint32_t>(i))
                   .set_ack(static_cast<uint32_t>(i + 1))
                   .set_flags(0x10)
                   .set_window(65535)
                   .set_payload(payload, PAYLOAD_SIZE);
            ssize_t len = builder.build(buffer, sizeof(buffer));
            do_not_optimize(len);
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("header_build.tcp_build_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  TCPBuilder::build (%zuB):          %6.1f ns/op\n",
                   PAYLOAD_SIZE, ns_per_op);
    }

    // --- TCPBuilder::build_header_only ---
    {
        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            TCPBuilder builder;
            builder.set_src_port(80)
                   .set_dst_port(12345)
                   .set_seq(static_cast<uint32_t>(i))
                   .set_ack(static_cast<uint32_t>(i + 1))
                   .set_flags(0x10)
                   .set_window(65535)
                   .set_payload(payload, PAYLOAD_SIZE);
            ssize_t len = builder.build_header_only(buffer, sizeof(buffer));
            do_not_optimize(len);
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("header_build.tcp_header_only_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  TCPBuilder::build_header_only:      %6.1f ns/op\n", ns_per_op);
    }

    // --- IPv4Builder::build (with payload copy) ---
    {
        uint8_t tcp_payload[PAYLOAD_SIZE + 20];
        std::memset(tcp_payload, 0xCD, sizeof(tcp_payload));

        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            IPv4Builder builder;
            builder.set_src(0x0A000001)
                   .set_dst(0x0A000002)
                   .set_protocol(6)
                   .set_ttl(64)
                   .set_payload(tcp_payload, PAYLOAD_SIZE);
            ssize_t len = builder.build(buffer, sizeof(buffer));
            do_not_optimize(len);
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("header_build.ipv4_build_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  IPv4Builder::build (%zuB):         %6.1f ns/op\n",
                   PAYLOAD_SIZE, ns_per_op);
    }

    // --- IPv4Builder::build_header_only ---
    {
        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            IPv4Builder builder;
            builder.set_src(0x0A000001)
                   .set_dst(0x0A000002)
                   .set_protocol(6)
                   .set_ttl(64);
            ssize_t len = builder.build_header_only(buffer, sizeof(buffer), PAYLOAD_SIZE);
            do_not_optimize(len);
        }
        auto end = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("header_build.ipv4_header_only_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  IPv4Builder::build_header_only:     %6.1f ns/op\n", ns_per_op);
    }

    if (!json_mode) printf("\n");
}

// ─────────────────────────────────────────────────────────
// Test 6: GlobalMetrics Hot Path
// ─────────────────────────────────────────────────────────

static void bench_global_metrics() {
    if (!json_mode) printf("[GlobalMetrics Hot Path]\n");

    constexpr uint64_t ITERS = 10000000;

    // --- Aligned (current implementation with alignas(64)) ---
    {
        GlobalMetrics metrics{};

        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            metrics.packets_rx.fetch_add(1, std::memory_order_relaxed);
            metrics.bytes_rx.fetch_add(64, std::memory_order_relaxed);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("global_metrics.aligned_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  aligned (current):    %6.1f ns/op\n", ns_per_op);
    }

    // --- Simulated unaligned (all counters packed in same cache line) ---
    {
        struct PackedMetrics {
            std::atomic<uint64_t> packets_rx{0};
            std::atomic<uint64_t> packets_tx{0};
            std::atomic<uint64_t> bytes_rx{0};
            std::atomic<uint64_t> bytes_tx{0};
            std::atomic<uint64_t> syn_received{0};
            std::atomic<uint64_t> syn_ack_sent{0};
            std::atomic<uint64_t> rst_received{0};
            std::atomic<uint64_t> rst_sent{0};
        };
        PackedMetrics packed{};

        auto start = Clock::now();
        for (uint64_t i = 0; i < ITERS; ++i) {
            packed.packets_rx.fetch_add(1, std::memory_order_relaxed);
            packed.bytes_rx.fetch_add(64, std::memory_order_relaxed);
        }
        auto end = Clock::now();

        double ns = std::chrono::duration<double, std::nano>(end - start).count();
        double ns_per_op = ns / ITERS;

        record("global_metrics.unaligned_ns_per_op", ns_per_op);

        if (!json_mode)
            printf("  simulated unaligned:  %6.1f ns/op\n", ns_per_op);
    }

    if (!json_mode) printf("\n");
}

// ─────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Parse --json flag
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json_mode = true;
        }
    }

    if (!json_mode)
        printf("=== NeuStack AF_XDP Datapath Benchmark ===\n\n");

#ifdef NEUSTACK_PLATFORM_LINUX
    bench_umem_alloc_free();
    bench_xdp_ring();
#else
    if (!json_mode) {
        printf("[UMEM Frame Alloc/Free]\n");
        printf("  (skipped: Linux only)\n\n");
        printf("[XDP Ring Operations]\n");
        printf("  (skipped: Linux only)\n\n");
    }
#endif

    bench_zero_copy();
    bench_prefetch();
    bench_header_build();
    bench_global_metrics();

    if (json_mode) {
        print_json_results();
    } else {
        printf("=== Done ===\n");
    }

    return 0;
}
