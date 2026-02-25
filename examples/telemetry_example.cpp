/**
 * @file telemetry_example.cpp
 * @brief NeuStack 使用示例 - Prometheus Metrics 与 Telemetry API
 *
 * 编译后以 root 运行：sudo ./telemetry_example
 * 另一个终端：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   curl http://192.168.100.2/api/v1/stats
 *   curl http://192.168.100.2/metrics
 */
#include "neustack/neustack.hpp"
#include <iostream>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    auto stack = NeuStack::create(cfg);

    auto& server = stack->http_server();

    // 1. 提供标准的业务 API
    server.get("/api/hello", [](const HttpRequest&) {
        return HttpResponse().set_body("Hello! Check /metrics for telemetry data.\n");
    });

    // 2. 导出 JSON 格式的 Telemetry 统计信息
    server.get("/api/v1/stats", [&stack](const HttpRequest&) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(stack->status_json(true));
    });

    // 3. 导出 Prometheus 格式的 Metrics
    server.get("/metrics", [&stack](const HttpRequest&) {
        std::string metrics_data = stack->status_prometheus();
        
        return HttpResponse()
            .content_type("text/plain")
            .set_body(metrics_data);
    });

    server.listen(80);

    std::cout << "Telemetry Server Started.\n";
    std::cout << "Try:\n";
    std::cout << "  curl http://192.168.100.2/api/v1/stats\n";
    std::cout << "  curl http://192.168.100.2/metrics\n";

    stack->run();
}
