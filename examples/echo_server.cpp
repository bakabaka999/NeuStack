/**
 * @file    echo_server.cpp
 * @brief   NeuStack example — TCP Echo Server
 *
 * Demonstrates:
 *   - Listening on a TCP port
 *   - Handling incoming connections via callback
 *   - Echoing received data back to the client
 *
 * Build & run:
 *   cmake --build build --target echo_server
 *   sudo ./build/examples/echo_server
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   nc 192.168.100.2 7000
 *   (type any text — it is echoed back verbatim)
 */

#include "neustack/neustack.hpp"

#include <cstdio>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    stack->tcp().listen(7000, [](IStreamConnection *conn) -> StreamCallbacks {
        std::printf("Connection from %s\n",
                    ip_to_string(conn->remote_ip()).c_str());

        return {
            .on_receive = [](IStreamConnection *c, const uint8_t *data, size_t len) {
                c->send(data, len);   // echo verbatim
            },
            .on_close = [](IStreamConnection *c) {
                std::printf("Connection closed\n");
                c->close();
            },
        };
    });

    std::printf("TCP Echo Server listening on port 7000\n");
    std::printf("Test with: nc 192.168.100.2 7000\n");

    stack->run();
}
