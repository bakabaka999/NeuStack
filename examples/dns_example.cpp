/**
 * @file dns_example.cpp
 * @brief NeuStack 使用示例 - DNS 异步解析
 *
 * 编译后以 root 运行：sudo ./dns_example
 * 另一个终端：
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
    cfg.dns_server = ip_from_string("8.8.8.8");       // 默认 Google DNS

    auto stack = NeuStack::create(cfg);

    if (!stack->dns()) {
        std::cerr << "DNS client not enabled!\n";
        return 1;
    }

    std::string domain = "github.com";
    std::cout << "Resolving " << domain << " asynchronously...\n";

    // 由于 stack->run() 是阻塞的，我们在启动前注册一个延时任务来发起 DNS 请求
    // 也可以直接在线程里发起请求
    std::thread([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        stack->dns()->resolve_async(domain, [domain](std::optional<DNSResponse> resp) {
            if (!resp || resp->rcode != DNSRcode::NoError) {
                std::cerr << "DNS lookup failed for " << domain << "\n";
            } else {
                auto ip = resp->get_ip();
                if (ip) {
                    std::cout << "DNS Result: " << domain << " -> " << ip_to_string(*ip) << "\n";
                } else {
                    std::cout << "DNS Result: " << domain << " has no A record.\n";
                }
            }
            std::cout << "Press Ctrl+C to exit.\n";
        });
    }).detach();

    stack->run(); // 阻塞运行网络栈
}
