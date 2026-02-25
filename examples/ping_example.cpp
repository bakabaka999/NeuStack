/**
 * @file ping_example.cpp
 * @brief NeuStack 使用示例 - ICMP Ping
 *
 * 编译后以 root 运行：sudo ./ping_example
 * 另一个终端（配置网卡路由并测试外网 ping）：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   sudo ./scripts/nat/setup_nat.sh --dev utun9
 */
#include "neustack/neustack.hpp"
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <mutex>

using namespace neustack;
using namespace std::chrono;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";

    auto stack = NeuStack::create(cfg);

    if (!stack->icmp()) {
        std::cerr << "ICMP not enabled in the stack!\n";
        return 1;
    }

    uint32_t target_ip = ip_from_string("8.8.8.8");
    uint16_t ping_id = 1234;
    
    std::mutex mtx;
    std::unordered_map<uint16_t, steady_clock::time_point> sent_times;

    // 注册 ICMP Echo Reply 的回调函数
    stack->icmp()->set_echo_reply_callback(
        [&](uint32_t src, uint16_t id, uint16_t seq, uint32_t) {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = sent_times.find(seq);
            if (it != sent_times.end()) {
                auto now = steady_clock::now();
                auto rtt = duration_cast<milliseconds>(now - it->second).count();
                std::cout << "Reply from " << ip_to_string(src) 
                          << ": seq=" << seq << " time=" << rtt << " ms\n";
                sent_times.erase(it);
            }
        });

    std::cout << "PING " << ip_to_string(target_ip) << "...\n";

    // 启动一个后台线程来发送 Ping 请求（因为 stack->run() 会阻塞主线程）
    std::thread ping_thread([&]() {
        for (uint16_t seq = 1; seq <= 4; ++seq) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            {
                std::lock_guard<std::mutex> lock(mtx);
                sent_times[seq] = steady_clock::now();
            }

            const char payload[] = "NeuStack Ping";
            stack->icmp()->send_echo_request(target_ip, ping_id, seq, 
                                             reinterpret_cast<const uint8_t*>(payload), 
                                             sizeof(payload) - 1);
        }
    });

    stack->run(); // 阻塞运行，处理网络事件
    ping_thread.join();
    return 0;
}
