/**
 * @file    fw_example.cpp
 * @brief   NeuStack example — Firewall rules and shadow mode
 *
 * Demonstrates:
 *   - IP blacklist / whitelist
 *   - Port-blocking rules
 *   - Packet rate limiting
 *   - Shadow mode (alert-only, no drops)
 *   - Reading firewall stats at runtime
 *
 * Build & run:
 *   cmake --build build --target fw_example
 *   sudo ./build/examples/fw_example
 *
 * Setup (once per boot, in another terminal):
 *   sudo ./scripts/nat/setup_nat.sh --dev <utunX>
 *
 * Test:
 *   curl http://192.168.100.2/              # allowed
 *   curl http://192.168.100.2:8080/         # blocked by rule #1
 *   curl http://192.168.100.2/stats         # live firewall stats
 */

#include "neustack/neustack.hpp"

#include <cstdio>
#include <string>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip          = "192.168.100.2";
    cfg.enable_firewall   = true;
    cfg.firewall_shadow_mode = false;   // enforce mode — actually drop packets

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    // ── Configure firewall rules ──────────────────────────────────────────────
    auto *rules = stack->firewall_rules();  // non-null when firewall is enabled

    // 1. Rate limit: max 200 pps, burst up to 400
    rules->rate_limiter().set_rate(200, 400);
    rules->rate_limiter().set_enabled(true);

    // 2. Blacklist a specific IP
    rules->add_blacklist_ip(ip_from_string("10.0.0.99"));

    // 3. Block TCP port 8080
    rules->add_rule(Rule::block_port(1, 8080, 6 /* TCP */));

    // 4. Block UDP port 53 inbound (we handle DNS ourselves)
    rules->add_rule(Rule::block_port(2, 53, 17 /* UDP */));

    std::printf("Firewall armed (enforce mode)\n");
    std::printf("  Rate limit : 200 pps  (burst 400)\n");
    std::printf("  Blacklisted: 10.0.0.99\n");
    std::printf("  Blocked    : TCP/8080, UDP/53\n\n");

    // ── HTTP server ──────────────────────────────────────────────────────────
    auto &srv = stack->http_server();

    srv.get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("Protected by NeuStack Firewall\n");
    });

    // Live firewall stats endpoint
    srv.get("/stats", [&stack](const HttpRequest &) {
        const auto  s     = stack->firewall_stats();
        const auto *rules = stack->firewall_rules();

        std::string json = "{";
        json += "\"shadow_mode\":"     + std::string(stack->firewall_shadow_mode() ? "true" : "false") + ",";
        json += "\"packets_inspected\":" + std::to_string(s.packets_inspected) + ",";
        json += "\"packets_passed\":"    + std::to_string(s.packets_passed)    + ",";
        json += "\"packets_dropped\":"   + std::to_string(s.packets_dropped)   + ",";
        json += "\"packets_alerted\":"   + std::to_string(s.packets_alerted);

        if (rules) {
            const auto &st = rules->stats();
            json += ",\"rule_engine\":{";
            json += "\"whitelist_hits\":"   + std::to_string(st.whitelist_hits)   + ",";
            json += "\"blacklist_hits\":"   + std::to_string(st.blacklist_hits)   + ",";
            json += "\"rate_limit_drops\":" + std::to_string(st.rate_limit_drops) + ",";
            json += "\"rule_matches\":"     + std::to_string(st.rule_matches)     + ",";
            json += "\"default_passes\":"   + std::to_string(st.default_passes);
            json += "}";
        }
        json += "}";

        return HttpResponse()
            .content_type("application/json")
            .set_body(json);
    });

    // Toggle shadow mode at runtime via /shadow?mode=on|off
    srv.get("/shadow", [&stack](const HttpRequest &req) {
        std::string mode = req.query_param("mode");
        if (mode == "on") {
            stack->firewall_set_shadow_mode(true);
            return HttpResponse().set_body("Shadow mode ON (alert only)\n");
        } else if (mode == "off") {
            stack->firewall_set_shadow_mode(false);
            return HttpResponse().set_body("Shadow mode OFF (enforce)\n");
        }
        HttpResponse resp;
        resp.status = HttpStatus::BadRequest;
        resp.set_body("Usage: /shadow?mode=on|off\n");
        return resp;
    });

    srv.listen(80);

    std::printf("HTTP server on http://%s\n", cfg.local_ip.c_str());
    std::printf("  GET /          — protected page\n");
    std::printf("  GET /stats     — live firewall counters\n");
    std::printf("  GET /shadow?mode=on|off — toggle shadow mode\n");

    stack->run();
}
