/**
 * @file    http_client.cpp
 * @brief   NeuStack example — Async HTTP client
 *
 * Demonstrates:
 *   - Issuing an outbound HTTP GET request
 *   - Handling the async response (status, headers, body)
 *
 * Build & run:
 *   cmake --build build --target http_client
 *   sudo ./build/examples/http_client
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Press Ctrl+C to exit after the response is printed.
 */

#include "neustack/neustack.hpp"

#include <cstdio>
#include <thread>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    // Fire the request after the stack has started (1-second head-start).
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint32_t dst = ip_from_string("1.1.1.1");
        std::printf("GET http://1.1.1.1/\n");

        stack->http_client().get(dst, 80, "/", [](const HttpResponse &resp, int err) {
            if (err != 0) {
                std::printf("[!] HTTP GET failed (err=%d)\n", err);
                return;
            }
            std::printf("\n--- Response ---\n");
            std::printf("Status : %d %s\n",
                        static_cast<int>(resp.status),
                        http_status_text(resp.status));
            for (const auto &[k, vals] : resp.headers)
                for (const auto &v : vals)
                    std::printf("%s: %s\n", k.c_str(), v.c_str());
            std::printf("\n%s\n", resp.body.c_str());
            std::printf("Press Ctrl+C to exit.\n");
        });
    }).detach();

    stack->run();
}
