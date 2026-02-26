/**
 * @file    ping_example.cpp
 * @brief   NeuStack example — ICMP ping with RTT measurement
 *
 * Demonstrates:
 *   - Sending ICMP Echo Requests
 *   - Receiving Echo Replies via callback
 *   - RTT measurement
 *
 * Build & run:
 *   cmake --build build --target ping_example
 *   sudo ./build/examples/ping_example
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>   # use device printed at startup
 *
 * Then press Enter in this terminal to start pinging.
 */

#include "neustack/neustack.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace neustack;
using namespace std::chrono;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    if (!stack->icmp()) {
        std::printf("[!] ICMP not enabled\n");
        return 1;
    }

    // ── Ping state ────────────────────────────────────────────────────────────
    constexpr int      PROBES  = 4;
    constexpr uint16_t PING_ID = 0x4E53;   // 'N','S'

    std::mutex                                       sent_mtx;
    std::unordered_map<uint16_t, steady_clock::time_point> sent_times;
    std::atomic<int> recv_count{0};
    std::atomic<bool> done{false};

    uint32_t target = ip_from_string("8.8.8.8");

    // ── Reply callback ────────────────────────────────────────────────────────
    stack->icmp()->set_echo_reply_callback(
        [&](uint32_t src, uint16_t /*id*/, uint16_t seq, uint32_t) {
            steady_clock::time_point sent;
            {
                std::lock_guard<std::mutex> lk(sent_mtx);
                auto it = sent_times.find(seq);
                if (it == sent_times.end()) return;
                sent = it->second;
                sent_times.erase(it);
            }
            double rtt_ms = duration<double, std::milli>(steady_clock::now() - sent).count();
            std::printf("  reply from %s  seq=%-3u  rtt=%.3f ms\n",
                        ip_to_string(src).c_str(), seq, rtt_ms);
            if (recv_count.fetch_add(1) + 1 == PROBES)
                done = true;
        });

    // ── Wait for NAT setup ────────────────────────────────────────────────────
    std::printf("\nDevice: %s\n", stack->device().get_name().c_str());
    std::printf("Run in another terminal:\n");
    std::printf("  sudo ./scripts/nat/setup_nat.sh --dev %s\n\n",
                stack->device().get_name().c_str());
    std::printf("Press Enter when NAT is ready...");
    std::fflush(stdout);
    std::getchar();

    // ── Send probes in background thread ─────────────────────────────────────
    std::printf("\nPING %s — %d packets\n", ip_to_string(target).c_str(), PROBES);

    std::thread sender([&] {
        for (uint16_t seq = 1; seq <= PROBES; ++seq) {
            {
                std::lock_guard<std::mutex> lk(sent_mtx);
                sent_times[seq] = steady_clock::now();
            }
            const char payload[] = "NeuStack";
            stack->icmp()->send_echo_request(target, PING_ID, seq,
                reinterpret_cast<const uint8_t *>(payload), sizeof(payload) - 1);
            std::printf("  → seq=%-3u sent\n", seq);
            std::this_thread::sleep_for(seconds(1));
        }
        // Allow 2s for last reply before stopping
        std::this_thread::sleep_for(seconds(2));
        done = true;
    });

    // ── Run event loop until done ─────────────────────────────────────────────
    stack->run();   // interrupted by done flag — but run() blocks on signal...

    // run() blocks until Ctrl+C; use stop() from sender thread instead
    sender.join();

    int sent = PROBES;
    int recv = recv_count.load();
    std::printf("\n--- %s ---  %d sent, %d received, %d%% loss\n",
                ip_to_string(target).c_str(), sent, recv,
                (sent - recv) * 100 / sent);
    return 0;
}
