/**
 * @file echo_server.cpp
 * @brief NeuStack 使用示例 - TCP Echo 服务器
 *
 * 编译后以 root 运行：sudo ./echo_server
 * 另一个终端：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   nc 192.168.100.2 7000
 *   输入任何文本，都会原样返回。
 */
#include "neustack/neustack.hpp"
#include <iostream>

using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    auto stack = NeuStack::create(cfg);

    // 监听 TCP 端口 7000
    stack->tcp().listen(7000, [](IStreamConnection *conn) -> StreamCallbacks {
        std::cout << "New client connected to Echo Server!\n";
        
        return {
            // 当接收到数据时触发
            .on_receive = [](IStreamConnection *c, const uint8_t *data, size_t len) {
                // 将接收到的数据原样发回去
                c->send(data, len);
            },
            // 当连接关闭时触发
            .on_close = [](IStreamConnection *c) {
                std::cout << "Client disconnected.\n";
                c->close();
            }
        };
    });

    std::cout << "TCP Echo Server listening on port 7000.\n";
    std::cout << "Test with: nc 192.168.100.2 7000\n";

    stack->run(); // 阻塞运行
}
