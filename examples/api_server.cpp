#include "neustack/neustack.hpp"
#include <iostream>
#include <vector>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    
    auto stack = NeuStack::create(cfg);

    // ==========================================
    // Asynchronous Request Aggregator
    // Fetches multiple internal APIs and aggregates the result
    // ==========================================
    stack->http_server().get("/api/dashboard", [](const HttpRequest&) {
        
        // This is a naive synchronous mock implementation since true async 
        // request aggregation requires chaining callbacks or using coroutines
        // which makes the example too complex.
        
        std::string json = "{";
        json += "\"service\": \"Dashboard Aggregator\",";
        json += "\"status\": \"Online\",";
        json += "\"components\": [";
        json += "{\"name\": \"Web\", \"status\": \"OK\"},";
        json += "{\"name\": \"Database\", \"status\": \"OK\"}";
        json += "]";
        json += "}";
        
        return HttpResponse()
            .content_type("application/json")
            .set_body(json);
    });
    
    // An API endpoint with URL parameters
    stack->http_server().get("/api/user", [](const HttpRequest& req) {
        std::string id = req.get_header("User-Id");
        if (id.empty()) id = "Unknown";
        
        return HttpResponse()
            .content_type("application/json")
            .set_body("{\"user_id\": \"" + id + "\", \"role\": \"admin\"}\n");
    });

    stack->http_server().listen(80);

    std::cout << "Advanced API Server Started.\n";
    std::cout << "Try:\n";
    std::cout << "  curl http://192.168.100.2/api/dashboard\n";
    std::cout << "  curl -H 'User-Id: 42' http://192.168.100.2/api/user\n";

    stack->run();
}