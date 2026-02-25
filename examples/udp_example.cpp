/**
 * @file udp_example.cpp
 * @brief NeuStack 使用示例 - UDP 绑定与收发
 *
 * 编译后以 root 运行：sudo ./udp_example
 * 另一个终端：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   nc -u 192.168.100.2 8000
 *   输入一些内容，将会收到带前缀的 UDP 回复。
 */
#include "neustack/neustack.hpp"
#include <iostream>
#include <string>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    auto stack = NeuStack::create(cfg);

    if (!stack->udp()) {
        std::cerr << "UDP is not enabled!\n";
        return 1;
    }

    uint16_t port = 8000;

    // 绑定 UDP 端口 8000 并设置接收回调
    stack->udp()->bind(port, [&stack](uint32_t src_ip, uint16_t src_port,
                                      const uint8_t *data, size_t len) {
        
        std::string payload(reinterpret_cast<const char*>(data), len);
        std::cout << "Received UDP packet from " << ip_to_string(src_ip) 
                  << ":" << src_port << " -> " << payload << "\n";

        // 构造回复内容
        std::string reply = "UDP Server says: " + payload;

        // 发送 UDP 回复
        stack->udp()->sendto(src_ip, src_port, 8000, 
                             reinterpret_cast<const uint8_t*>(reply.c_str()), 
                             reply.length());
    });

    std::cout << "UDP Echo Server listening on port " << port << ".\n";
    std::cout << "Test with: nc -u 192.168.100.2 " << port << "\n";

    stack->run();
}
