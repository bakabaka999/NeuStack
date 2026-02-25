#include "neustack/neustack.hpp"
#include <iostream>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    
    auto stack = NeuStack::create(cfg);

    if (stack->firewall_enabled()) {
        auto* rules = stack->firewall_rules();

        // 1. Enable rate limiting (100 PPS, burst to 200)
        rules->rate_limiter().set_rate(100, 200);
        rules->rate_limiter().set_enabled(true);

        // 2. Add an IP to the blacklist
        rules->add_blacklist_ip(ip_from_string("192.168.100.5"));

        // 3. Block port 8080 (TCP = 6)
        rules->add_rule(Rule::block_port(1, 8080, 6));

        std::cout << "Firewall is armed.\n";
        std::cout << " - Rate Limit: 100 PPS\n";
        std::cout << " - Blacklisted IP: 192.168.100.5\n";
        std::cout << " - Blocked TCP Port: 8080\n";
    }

    // A web server protected by NeuStack firewall
    stack->http_server().get("/", [](const HttpRequest&) {
        return HttpResponse().set_body("This is protected by NeuStack Firewall!\n");
    });
    stack->http_server().listen(80);

    // This port is explicitly blocked by the firewall rule above
    stack->tcp().listen(8080, [](IStreamConnection *) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection *c, const uint8_t *d, size_t l) { c->send(d, l); },
            .on_close   = [](IStreamConnection *c) { c->close(); }
        };
    });

    std::cout << "Server running on http://192.168.100.2\n";
    stack->run(); 
}