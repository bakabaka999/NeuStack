/**
 * @file minimal.cpp
 * @brief NeuStack 最简使用示例 - HTTP Echo 服务器
 *
 * 编译后以 root 运行：sudo ./minimal
 * 另一个终端：
 *   sudo ifconfig utun9 192.168.100.1 192.168.100.2 up
 *   curl http://192.168.100.2/
 */
#include "neustack/neustack.hpp"

using namespace neustack;

int main() {
    auto stack = NeuStack::create(); // 默认配置：192.168.100.2

    // 注册一个 HTTP 路由
    stack->http_server().get("/", [](const HttpRequest &) { 
        return HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n"); 
    });
    stack->http_server().listen(80);

    stack->run(); // 阻塞运行，Ctrl+C 退出
}