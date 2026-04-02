/**
 * @file    https_server.cpp
 * @brief   HTTPS server example using NeuStack TLS support
 *
 * Build (requires mbedTLS in third_party/mbedtls):
 *   cmake -B build -DNEUSTACK_ENABLE_TLS=ON
 *   cmake --build build --target https_server
 *
 * Generate test certificate:
 *   bash scripts/tls/gen_self_signed_cert.sh
 *
 * Run:
 *   sudo ./build/examples/https_server
 *
 * Setup NAT (in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   curl -k https://192.168.100.2/
 *   curl -k https://192.168.100.2/api/status
 */

#include "neustack/neustack.hpp"

#include <cstdio>

using namespace neustack;

int main() {
#ifndef NEUSTACK_TLS_ENABLED
    std::fprintf(stderr, "Error: TLS support not compiled. "
                         "Rebuild with -DNEUSTACK_ENABLE_TLS=ON\n");
    return 1;
#else
    // 1. Configure stack with TLS certificates
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    cfg.tls_cert_path = "server_cert.pem";
    cfg.tls_key_path = "server_key.pem";
    cfg.log_level = LogLevel::INFO;

    auto stack = NeuStack::create(cfg);
    if (!stack) {
        std::fprintf(stderr, "Failed to create NeuStack\n");
        return 1;
    }

    // 2. Check that HTTPS server is available
    if (!stack->https_server()) {
        std::fprintf(stderr, "HTTPS server not initialized.\n"
                             "Make sure server_cert.pem and server_key.pem exist.\n"
                             "Generate with: bash scripts/tls/gen_self_signed_cert.sh\n");
        return 1;
    }

    // 3. Register routes on the HTTPS server
    stack->https_server()->get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/html")
            .set_body("<html><body>"
                      "<h1>Hello from NeuStack HTTPS!</h1>"
                      "<p>Secured with TLS via mbedTLS.</p>"
                      "</body></html>\n");
    });

    stack->https_server()->get("/api/status", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"status":"ok","tls":true})");
    });

    stack->https_server()->listen(443);

    // 4. Also serve plain HTTP on port 80 (optional)
    stack->http_server().get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("This is plain HTTP. Use https://192.168.100.2/ for TLS.\n");
    });
    stack->http_server().listen(80);

    std::printf("HTTPS server listening on https://192.168.100.2/\n");
    std::printf("HTTP  server listening on http://192.168.100.2/\n");

    // 5. Run the event loop
    stack->run();

    return 0;
#endif
}
