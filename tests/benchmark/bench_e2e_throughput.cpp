/**
 * bench_e2e_throughput.cpp — 端到端吞吐量测试
 *
 * 测量 NeuStack HAL 层 (TUN / AF_XDP) 的包收发吞吐量。
 * 配合 scripts/bench/run_throughput_test.sh 使用，
 * 该脚本会设置 veth 对和 network namespace。
 *
 * 两种模式：
 *   --mode sink   : 接收端，计数收到的包并报告 pps
 *   --mode flood  : 发送端，尽可能快地发 UDP 包
 *
 * Usage:
 *   ./bench_e2e_throughput --mode sink --device tun --duration 10
 *   ./bench_e2e_throughput --mode flood --device tun --target-ip 10.0.0.2 --duration 10
 *   ./bench_e2e_throughput --mode sink --device af_xdp --ifname veth1 --duration 10
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
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

// ─────────────────────────────────────────────────────────
// UDP packet builder (minimal, for flood mode)
// ─────────────────────────────────────────────────────────

static size_t build_udp_packet(uint8_t* buf, size_t payload_size,
                                uint32_t src_ip, uint32_t dst_ip,
                                uint16_t src_port, uint16_t dst_port) {
    // IP header
    auto* ip = reinterpret_cast<struct iphdr*>(buf);
    size_t total_len = sizeof(struct iphdr) + sizeof(struct udphdr) + payload_size;

    ip->ihl = 5;
    ip->version = 4;
    ip->tos = 0;
    ip->tot_len = htons(static_cast<uint16_t>(total_len));
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check = 0;
    ip->saddr = htonl(src_ip);
    ip->daddr = htonl(dst_ip);

    // Simple IP checksum
    uint32_t sum = 0;
    auto* hdr16 = reinterpret_cast<uint16_t*>(ip);
    for (int i = 0; i < 10; ++i) sum += ntohs(hdr16[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = htons(static_cast<uint16_t>(~sum));

    // UDP header
    auto* udp = reinterpret_cast<struct udphdr*>(buf + sizeof(struct iphdr));
    udp->source = htons(src_port);
    udp->dest = htons(dst_port);
    udp->len = htons(static_cast<uint16_t>(sizeof(struct udphdr) + payload_size));
    udp->check = 0; // skip for benchmark

    // Payload (fill with pattern)
    std::memset(buf + sizeof(struct iphdr) + sizeof(struct udphdr), 0xAA, payload_size);

    return total_len;
}

// ─────────────────────────────────────────────────────────
// Sink mode: receive and count packets
// ─────────────────────────────────────────────────────────

struct SinkStats {
    uint64_t packets;
    uint64_t bytes;
    double duration_s;
};

static SinkStats run_sink(NetDevice& dev, int duration_s) {
    SinkStats stats{};
    uint8_t buf[4096];
    PacketDesc descs[64];

    bool use_batch = dev.supports_batch();

    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(duration_s);

    while (g_running) {
        auto now = Clock::now();
        if (now >= deadline) break;

        if (use_batch) {
            uint32_t n = dev.recv_batch(descs, 64);
            for (uint32_t i = 0; i < n; ++i) {
                stats.packets++;
                stats.bytes += descs[i].len;
            }
            if (n > 0) dev.release_rx(descs, n);
        } else {
            ssize_t n = dev.recv(buf, sizeof(buf), 10);
            if (n > 0) {
                stats.packets++;
                stats.bytes += static_cast<uint64_t>(n);
            }
        }
    }

    auto end = Clock::now();
    stats.duration_s = std::chrono::duration<double>(end - start).count();
    return stats;
}

// ─────────────────────────────────────────────────────────
// Flood mode: send UDP packets as fast as possible
// ─────────────────────────────────────────────────────────

struct FloodStats {
    uint64_t packets;
    uint64_t bytes;
    double duration_s;
};

static FloodStats run_flood(NetDevice& dev, int duration_s,
                             uint32_t target_ip, size_t payload_size) {
    FloodStats stats{};
    uint8_t pkt[2048];

    // Pre-build packet
    uint32_t src_ip = 0x0A000001; // 10.0.0.1
    size_t pkt_len = build_udp_packet(pkt, payload_size,
                                       src_ip, target_ip, 9999, 9999);

    auto start = Clock::now();
    auto deadline = start + std::chrono::seconds(duration_s);

    // For batch mode
    PacketDesc descs[64];
    bool use_batch = dev.supports_batch();

    if (use_batch) {
        for (int i = 0; i < 64; ++i) {
            descs[i].data = pkt;
            descs[i].len = static_cast<uint32_t>(pkt_len);
            descs[i].addr = 0;
            descs[i].port_id = 0;
            descs[i].flags = 0;
        }
    }

    while (g_running) {
        auto now = Clock::now();
        if (now >= deadline) break;

        if (use_batch) {
            uint32_t sent = dev.send_batch(descs, 64);
            stats.packets += sent;
            stats.bytes += sent * pkt_len;
        } else {
            ssize_t n = dev.send(pkt, pkt_len);
            if (n > 0) {
                stats.packets++;
                stats.bytes += static_cast<uint64_t>(n);
            }
        }
    }

    auto end = Clock::now();
    stats.duration_s = std::chrono::duration<double>(end - start).count();
    return stats;
}

// ─────────────────────────────────────────────────────────
// JSON output
// ─────────────────────────────────────────────────────────

static void print_json(const char* mode, const char* device_type,
                        uint64_t packets, uint64_t bytes, double duration_s) {
    double pps = packets / duration_s;
    double mpps = pps / 1e6;
    double gbps = (bytes * 8.0) / duration_s / 1e9;

    printf("{\n");
    printf("  \"benchmark\": \"e2e_throughput\",\n");
    printf("  \"mode\": \"%s\",\n", mode);
    printf("  \"device\": \"%s\",\n", device_type);
    printf("  \"duration_s\": %.3f,\n", duration_s);
    printf("  \"results\": {\n");
    printf("    \"packets\": %" PRIu64 ",\n", packets);
    printf("    \"bytes\": %" PRIu64 ",\n", bytes);
    printf("    \"pps\": %.0f,\n", pps);
    printf("    \"mpps\": %.6f,\n", mpps);
    printf("    \"gbps\": %.6f\n", gbps);
    printf("  }\n");
    printf("}\n");
}

static void print_human(const char* mode, const char* device_type,
                         uint64_t packets, uint64_t bytes, double duration_s) {
    double pps = packets / duration_s;
    double mpps = pps / 1e6;
    double gbps = (bytes * 8.0) / duration_s / 1e9;

    printf("=== E2E Throughput: %s mode, %s device ===\n", mode, device_type);
    printf("  Duration:   %.1f s\n", duration_s);
    printf("  Packets:    %" PRIu64 "\n", packets);
    printf("  Throughput: %.3f Mpps (%.3f Gbps)\n", mpps, gbps);
    printf("=== Done ===\n");
}

// ─────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────

static void usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --mode <sink|flood>     Operation mode (required)\n");
    printf("  --device <tun|af_xdp>   Device type (default: tun)\n");
    printf("  --ifname <name>         Interface name for AF_XDP (default: veth1)\n");
    printf("  --target-ip <ip>        Target IP for flood mode (default: 10.0.0.2)\n");
    printf("  --duration <seconds>    Test duration (default: 10)\n");
    printf("  --payload <bytes>       UDP payload size (default: 64)\n");
    printf("  --json                  JSON output\n");
}

int main(int argc, char* argv[]) {
    std::string mode;
    std::string device_type = "tun";
    std::string ifname = "veth1";
    std::string target_ip_str = "10.0.0.2";
    int duration = 10;
    size_t payload_size = 64;
    bool json_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--device" && i + 1 < argc) device_type = argv[++i];
        else if (arg == "--ifname" && i + 1 < argc) ifname = argv[++i];
        else if (arg == "--target-ip" && i + 1 < argc) target_ip_str = argv[++i];
        else if (arg == "--duration" && i + 1 < argc) duration = std::atoi(argv[++i]);
        else if (arg == "--payload" && i + 1 < argc) payload_size = std::atoi(argv[++i]);
        else if (arg == "--json") json_mode = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
    }

    if (mode.empty()) {
        fprintf(stderr, "Error: --mode is required (sink or flood)\n");
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create device
    std::unique_ptr<NetDevice> dev;

    if (device_type == "tun") {
        dev = NetDevice::create("tun");
    }
#if defined(NEUSTACK_PLATFORM_LINUX) && defined(NEUSTACK_ENABLE_AF_XDP)
    else if (device_type == "af_xdp") {
        AFXDPConfig cfg;
        cfg.ifname = ifname;
        cfg.zero_copy = false;           // generic mode for veth
        cfg.force_native_mode = false;   // SKB mode
        cfg.batch_size = 64;
        dev = std::make_unique<LinuxAFXDPDevice>(cfg);
    }
#endif
    else {
        fprintf(stderr, "Error: unknown device type '%s'\n", device_type.c_str());
        return 1;
    }

    if (!dev) {
        fprintf(stderr, "Error: failed to create %s device\n", device_type.c_str());
        return 1;
    }

    if (dev->open() != 0) {
        fprintf(stderr, "Error: failed to open %s device\n", device_type.c_str());
        return 1;
    }

    if (mode == "sink") {
        auto stats = run_sink(*dev, duration);
        if (json_mode)
            print_json("sink", device_type.c_str(), stats.packets, stats.bytes, stats.duration_s);
        else
            print_human("sink", device_type.c_str(), stats.packets, stats.bytes, stats.duration_s);
    }
    else if (mode == "flood") {
        struct in_addr addr;
        inet_aton(target_ip_str.c_str(), &addr);
        uint32_t target_ip = ntohl(addr.s_addr);

        auto stats = run_flood(*dev, duration, target_ip, payload_size);
        if (json_mode)
            print_json("flood", device_type.c_str(), stats.packets, stats.bytes, stats.duration_s);
        else
            print_human("flood", device_type.c_str(), stats.packets, stats.bytes, stats.duration_s);
    }
    else {
        fprintf(stderr, "Error: unknown mode '%s'\n", mode.c_str());
        return 1;
    }

    dev->close();
    return 0;
}
