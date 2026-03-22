/**
 * bench_e2e_throughput.cpp — 端到端吞吐量测试
 *
 * 测量不同包收发后端的吞吐量：
 *   raw_socket: AF_PACKET recvfrom()，每包一次系统调用（模拟 TUN 路径）
 *   af_xdp:     AF_XDP ring buffer 批量收发（零拷贝路径）
 *
 * 两种后端都绑定到同一个 veth 接口，公平对比。
 * 配合 scripts/bench/run_throughput_test.sh 使用。
 *
 * Usage:
 *   ./bench_e2e_throughput --mode sink --device raw_socket --ifname veth0 --duration 10
 *   ./bench_e2e_throughput --mode sink --device af_xdp --ifname veth0 --duration 10
 *   ./bench_e2e_throughput --mode sink --device raw_socket --ifname veth0 --duration 10 --json
 */

#include "neustack/hal/device.hpp"

#ifdef NEUSTACK_PLATFORM_LINUX
#ifdef NEUSTACK_ENABLE_AF_XDP
#include "neustack/hal/hal_linux_afxdp.hpp"
#endif
#endif

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>

#ifdef __linux__
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#endif

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

// ─────────────────────────────────────────────────────────
// Sink: AF_PACKET raw socket (baseline, simulates TUN)
// One recvfrom() per packet — same overhead as read() on /dev/net/tun
// ─────────────────────────────────────────────────────────

struct SinkStats {
    uint64_t packets;
    uint64_t bytes;
    double duration_s;
};

#ifdef __linux__
static SinkStats run_sink_raw_socket(const std::string& ifname, int duration_s) {
    SinkStats stats{};

    // Create raw AF_PACKET socket
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket(AF_PACKET)");
        return stats;
    }

    // Bind to specific interface
    unsigned int ifindex = if_nametoindex(ifname.c_str());
    if (ifindex == 0) {
        fprintf(stderr, "Error: interface '%s' not found\n", ifname.c_str());
        ::close(fd);
        return stats;
    }

    struct sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = static_cast<int>(ifindex);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sll), sizeof(sll)) < 0) {
        perror("bind");
        ::close(fd);
        return stats;
    }

    uint8_t buf[2048];
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(duration_s);

    while (g_running) {
        if (Clock::now() >= deadline) break;

        int ret = poll(&pfd, 1, 100); // 100ms timeout
        if (ret <= 0) continue;

        ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr);
        if (n > 0) {
            stats.packets++;
            stats.bytes += static_cast<uint64_t>(n);
        }
    }

    auto end = Clock::now();
    stats.duration_s = std::chrono::duration<double>(end - start).count();
    ::close(fd);
    return stats;
}
#endif

// ─────────────────────────────────────────────────────────
// Sink: AF_XDP (batch ring buffer)
// ─────────────────────────────────────────────────────────

#if defined(NEUSTACK_PLATFORM_LINUX) && defined(NEUSTACK_ENABLE_AF_XDP)
static SinkStats run_sink_afxdp(const std::string& ifname, int duration_s) {
    SinkStats stats{};

    AFXDPConfig cfg;
    cfg.ifname = ifname;
    cfg.zero_copy = false;           // generic mode for veth
    cfg.force_native_mode = false;   // SKB mode
    cfg.batch_size = 64;
    cfg.bpf_prog_path = "";          // use SKB mode (no BPF program needed)

    LinuxAFXDPDevice dev(cfg);
    if (dev.open() != 0) {
        fprintf(stderr, "Error: failed to open AF_XDP on %s\n", ifname.c_str());
        return stats;
    }

    PacketDesc descs[64];

    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(duration_s);

    while (g_running) {
        if (Clock::now() >= deadline) break;

        uint32_t n = dev.recv_batch(descs, 64);
        for (uint32_t i = 0; i < n; ++i) {
            stats.packets++;
            stats.bytes += descs[i].len;
        }
        if (n > 0) dev.release_rx(descs, n);
    }

    auto end = Clock::now();
    stats.duration_s = std::chrono::duration<double>(end - start).count();
    dev.close();
    return stats;
}
#endif

// ─────────────────────────────────────────────────────────
// Output
// ─────────────────────────────────────────────────────────

static void print_results(const char* device_type, const SinkStats& stats, bool json) {
    double pps = stats.duration_s > 0 ? stats.packets / stats.duration_s : 0;
    double mpps = pps / 1e6;
    double gbps = stats.duration_s > 0 ? (stats.bytes * 8.0) / stats.duration_s / 1e9 : 0;

    if (json) {
        printf("{\n");
        printf("  \"benchmark\": \"e2e_throughput\",\n");
        printf("  \"device\": \"%s\",\n", device_type);
        printf("  \"duration_s\": %.3f,\n", stats.duration_s);
        printf("  \"results\": {\n");
        printf("    \"packets\": %" PRIu64 ",\n", stats.packets);
        printf("    \"bytes\": %" PRIu64 ",\n", stats.bytes);
        printf("    \"pps\": %.0f,\n", pps);
        printf("    \"mpps\": %.6f,\n", mpps);
        printf("    \"gbps\": %.6f\n", gbps);
        printf("  }\n");
        printf("}\n");
    } else {
        printf("=== E2E Throughput: %s ===\n", device_type);
        printf("  Duration:   %.1f s\n", stats.duration_s);
        printf("  Packets:    %" PRIu64 "\n", stats.packets);
        printf("  Throughput: %.4f Mpps (%.4f Gbps)\n", mpps, gbps);
        printf("=== Done ===\n");
    }
}

// ─────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────

static void usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --device <raw_socket|af_xdp>  Backend (default: raw_socket)\n");
    printf("  --ifname <name>               Interface to bind (required)\n");
    printf("  --duration <seconds>          Test duration (default: 10)\n");
    printf("  --json                        JSON output\n");
}

int main(int argc, char* argv[]) {
    std::string device_type = "raw_socket";
    std::string ifname;
    int duration = 10;
    bool json_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc) device_type = argv[++i];
        else if (arg == "--ifname" && i + 1 < argc) ifname = argv[++i];
        else if (arg == "--duration" && i + 1 < argc) duration = std::atoi(argv[++i]);
        else if (arg == "--json") json_mode = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
    }

    if (ifname.empty()) {
        fprintf(stderr, "Error: --ifname is required\n");
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    SinkStats stats{};

#ifdef __linux__
    if (device_type == "raw_socket") {
        stats = run_sink_raw_socket(ifname, duration);
    }
#if defined(NEUSTACK_ENABLE_AF_XDP)
    else if (device_type == "af_xdp") {
        stats = run_sink_afxdp(ifname, duration);
    }
#endif
    else {
        fprintf(stderr, "Error: unknown device type '%s'\n", device_type.c_str());
        return 1;
    }
#else
    fprintf(stderr, "Error: this benchmark requires Linux\n");
    return 1;
#endif

    print_results(device_type.c_str(), stats, json_mode);
    return 0;
}
