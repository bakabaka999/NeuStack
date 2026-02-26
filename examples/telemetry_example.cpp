/**
 * @file    telemetry_example.cpp
 * @brief   NeuStack example — Prometheus metrics and Telemetry API
 *
 * Demonstrates:
 *   - Exposing live stack metrics in JSON and Prometheus formats
 *   - Auto-registered telemetry endpoints (/api/v1/... and /metrics)
 *   - Registering a custom HTTP route alongside built-in endpoints
 *
 * Build & run:
 *   cmake --build build --target telemetry_example
 *   sudo ./build/examples/telemetry_example
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   curl http://192.168.100.2/api/v1/stats   | python3 -m json.tool
 *   curl http://192.168.100.2/metrics
 *   curl http://192.168.100.2/
 *
 * Auto-registered endpoints (no extra code needed):
 *   /api/v1/health
 *   /api/v1/stats
 *   /api/v1/stats/traffic
 *   /api/v1/stats/tcp
 *   /api/v1/stats/security
 *   /api/v1/connections
 *   /metrics              — Prometheus exposition format
 */

#include "neustack/neustack.hpp"

#include <cstdio>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    auto &srv = stack->http_server();

    // Custom root endpoint alongside the built-in telemetry ones.
    srv.get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("NeuStack is running.\n"
                      "  GET /api/v1/stats  — JSON snapshot\n"
                      "  GET /metrics       — Prometheus\n");
    });

    srv.listen(80);

    std::printf("Telemetry server on http://%s\n", cfg.local_ip.c_str());
    std::printf("  GET /              — info page\n");
    std::printf("  GET /api/v1/stats  — JSON telemetry snapshot\n");
    std::printf("  GET /metrics       — Prometheus exposition format\n");

    stack->run();
}
