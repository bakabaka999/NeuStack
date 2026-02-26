/**
 * @file    udp_example.cpp
 * @brief   NeuStack example — UDP echo server
 *
 * Demonstrates:
 *   - Binding a UDP port
 *   - Receiving UDP datagrams via callback
 *   - Sending a UDP reply to the source
 *
 * Build & run:
 *   cmake --build build --target udp_example
 *   sudo ./build/examples/udp_example
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   nc -u 192.168.100.2 8000
 *   (type any text — reply is prefixed with "echo: ")
 */

#include "neustack/neustack.hpp"

#include <cstdio>
#include <string>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    if (!stack->udp()) {
        std::printf("[!] UDP not enabled\n");
        return 1;
    }

    constexpr uint16_t PORT = 8000;

    stack->udp()->bind(PORT, [&stack](uint32_t src_ip, uint16_t src_port,
                                      const uint8_t *data, size_t len) {
        std::string payload(reinterpret_cast<const char *>(data), len);
        std::printf("UDP from %s:%u  [%zu bytes]  %.*s\n",
                    ip_to_string(src_ip).c_str(), src_port,
                    len, static_cast<int>(len), data);

        std::string reply = "echo: " + payload;
        stack->udp()->sendto(src_ip, src_port, PORT,
                             reinterpret_cast<const uint8_t *>(reply.c_str()),
                             reply.size());
    });

    std::printf("UDP echo server on port %u\n", PORT);
    std::printf("Test with: nc -u 192.168.100.2 %u\n", PORT);

    stack->run();
}
