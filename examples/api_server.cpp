/**
 * @file    api_server.cpp
 * @brief   NeuStack example — JSON REST API server
 *
 * Demonstrates:
 *   - Multiple HTTP routes (GET / POST)
 *   - Reading query parameters
 *   - Reading request body
 *   - Building JSON responses manually
 *   - Serving live stack telemetry via /api/v1/stats
 *
 * Build & run:
 *   cmake --build build --target api_server
 *   sudo ./build/examples/api_server
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   curl http://192.168.100.2/api/status
 *   curl http://192.168.100.2/api/greet?name=World
 *   curl -X POST -d '{"msg":"hello"}' http://192.168.100.2/api/echo
 *   curl http://192.168.100.2/api/v1/stats | python3 -m json.tool
 *   curl http://192.168.100.2/metrics
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

    auto &srv = stack->http_server();

    // ── GET /api/status ──────────────────────────────────────────────────────
    srv.get("/api/status", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"status":"ok","version":"1.4.0"})");
    });

    // ── GET /api/greet?name=<name> ───────────────────────────────────────────
    srv.get("/api/greet", [](const HttpRequest &req) {
        std::string name = req.query_param("name");
        if (name.empty()) name = "World";
        return HttpResponse()
            .content_type("application/json")
            .set_body("{\"greeting\":\"Hello, " + name + "!\"}");
    });

    // ── POST /api/echo ───────────────────────────────────────────────────────
    // Reflects the request body back as JSON
    srv.post("/api/echo", [](const HttpRequest &req) {
        std::string ct = req.get_header("Content-Type");
        if (ct.empty()) ct = "application/octet-stream";
        return HttpResponse()
            .content_type("application/json")
            .set_body("{\"echoed\":true,"
                      "\"content_type\":\"" + ct + "\","
                      "\"body_length\":" + std::to_string(req.body.size()) + ","
                      "\"body\":\"" + req.body + "\"}");
    });

    // ── GET /api/v1/stats (live telemetry) ───────────────────────────────────
    // These endpoints are auto-registered by NeuStack when the stack is created.
    // Listed here for documentation; no extra code needed.
    //
    //   /api/v1/health       — health check
    //   /api/v1/stats        — full JSON snapshot
    //   /api/v1/stats/traffic
    //   /api/v1/stats/tcp
    //   /api/v1/stats/security
    //   /api/v1/connections  — active TCP connections
    //   /metrics             — Prometheus exposition format

    srv.listen(80);

    std::printf("API server listening on http://%s\n", cfg.local_ip.c_str());
    std::printf("  GET  /api/status\n");
    std::printf("  GET  /api/greet?name=<name>\n");
    std::printf("  POST /api/echo\n");
    std::printf("  GET  /api/v1/stats   (live telemetry)\n");
    std::printf("  GET  /metrics         (Prometheus)\n");

    stack->run();
}
