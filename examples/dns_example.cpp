/**
 * @file    dns_example.cpp
 * @brief   NeuStack example — Async DNS resolution
 *
 * Demonstrates:
 *   - Configuring a DNS server
 *   - Issuing an async DNS query while the stack runs
 *   - Handling the result in a callback
 *
 * Build & run:
 *   cmake --build build --target dns_example
 *   sudo ./build/examples/dns_example
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Press Ctrl+C to exit after the result is printed.
 */

#include "neustack/neustack.hpp"

#include <cstdio>
#include <thread>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip   = "192.168.100.2";
    cfg.dns_server = ip_from_string("8.8.8.8");

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    if (!stack->dns()) {
        std::printf("[!] DNS client not enabled\n");
        return 1;
    }

    const std::string domain = "github.com";
    std::printf("Resolving %s ...\n", domain.c_str());

    // Fire the query after the stack is running (1-second head-start).
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        stack->dns()->resolve_async(domain, [domain](std::optional<DNSResponse> resp) {
            if (!resp || resp->rcode != DNSRcode::NoError) {
                std::printf("[!] DNS lookup failed for %s\n", domain.c_str());
                return;
            }
            auto ip = resp->get_ip();
            if (ip) {
                std::printf("  %s  ->  %s\n", domain.c_str(), ip_to_string(*ip).c_str());
            } else {
                std::printf("  %s  ->  (no A record)\n", domain.c_str());
            }
            std::printf("Press Ctrl+C to exit.\n");
        });
    }).detach();

    stack->run();
}
