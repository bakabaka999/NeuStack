/**
 * @file    minimal.cpp
 * @brief   Minimal NeuStack example — HTTP "Hello World"
 *
 * Build & run:
 *   cmake --build build --target minimal
 *   sudo ./build/examples/minimal
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>   # replace with device printed at startup
 *
 * Test:
 *   curl http://192.168.100.2/
 */

#include "neustack/neustack.hpp"

using namespace neustack;

int main() {
    // Create stack with default config (IP: 192.168.100.2)
    auto stack = NeuStack::create();
    if (!stack) return 1;

    // Register a single HTTP route
    stack->http_server().get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(80);

    stack->run();   // Blocks until Ctrl+C
}
