/**
 * @file http_client.cpp
 * @brief NeuStack 使用示例 - 异步 HTTP Client
 *
 * 编译后以 root 运行：sudo ./http_client
 * 另一个终端（需要配置 NAT 以访问公网）：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   sudo ./scripts/nat/setup_nat.sh --dev utun9
 */
#include "neustack/neustack.hpp"
#include <iostream>
#include <thread>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);

    std::thread([&]() {
        // 等待协议栈初始化完成
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 假设你要访问 1.1.1.1 的 HTTP 服务 (Cloudflare)
        uint32_t target_ip = ip_from_string("1.1.1.1");
        
        std::cout << "Sending HTTP GET request to http://1.1.1.1/\n";

        stack->http_client().get(target_ip, 80, "/", [](const HttpResponse& resp, int err) {
            if (err != 0) {
                std::cerr << "HTTP GET failed with error code: " << err << "\n";
                return;
            }

            std::cout << "\n--- HTTP Response ---\n";
            std::cout << "Status: " << static_cast<int>(resp.status) << " " 
                      << http_status_text(resp.status) << "\n";
            
            for (const auto& [k, vals] : resp.headers) {
                for (const auto& v : vals) {
                    std::cout << k << ": " << v << "\n";
                }
            }
            
            std::cout << "\nBody:\n" << resp.body << "\n";
            std::cout << "---------------------\n";
            std::cout << "Press Ctrl+C to exit.\n";
        });
    }).detach();

    stack->run();
}
