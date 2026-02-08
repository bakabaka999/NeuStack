/**
 * 集成测试：TCP 三次握手 + 数据回显
 *
 * 使用 MockDevice 实现内存互联，两个完整的 IPv4Layer + TCPLayer 实例
 * 通过内存管道通信，验证 TCP 连接建立和数据传输。
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/transport/stream.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <queue>
#include <string>

using namespace neustack;

// ============================================================================
// MockDevice — 内存网络设备
// ============================================================================

class MockDevice : public NetDevice {
public:
    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock0"; }

    ssize_t send(const uint8_t* data, size_t len) override {
        // 发出的包存入 outbox，由测试循环转交给对端
        _outbox.push(std::vector<uint8_t>(data, data + len));
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t* buf, size_t len, int /*timeout_ms*/) override {
        if (_inbox.empty()) return 0;
        auto& pkt = _inbox.front();
        size_t n = std::min(len, pkt.size());
        std::memcpy(buf, pkt.data(), n);
        _inbox.pop();
        return static_cast<ssize_t>(n);
    }

    // 取出所有待发送的包
    bool has_output() const { return !_outbox.empty(); }

    std::vector<uint8_t> pop_output() {
        auto pkt = std::move(_outbox.front());
        _outbox.pop();
        return pkt;
    }

    // 注入一个包到收件箱
    void inject(const std::vector<uint8_t>& pkt) {
        _inbox.push(pkt);
    }

private:
    std::queue<std::vector<uint8_t>> _outbox;
    std::queue<std::vector<uint8_t>> _inbox;
};

// ============================================================================
// 测试辅助：在两端之间转发所有包
// ============================================================================

static void transfer_packets(MockDevice& from, IPv4Layer& to_ip) {
    while (from.has_output()) {
        auto pkt = from.pop_output();
        to_ip.on_receive(pkt.data(), pkt.size());
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // 降低日志级别，减少输出噪音
    auto& logger = Logger::instance();
    logger.set_level(LogLevel::WARN);

    std::printf("=== TCP Handshake Integration Test ===\n");

    // --- 1. 构建两端 ---
    MockDevice server_dev, client_dev;

    IPv4Layer server_ip(server_dev);
    IPv4Layer client_ip(client_dev);

    uint32_t server_addr = ip_from_string("192.168.100.1");
    uint32_t client_addr = ip_from_string("192.168.100.2");

    server_ip.set_local_ip(server_addr);
    client_ip.set_local_ip(client_addr);

    TCPLayer server_tcp(server_ip, server_addr);
    TCPLayer client_tcp(client_ip, client_addr);

    // 注册 TCP 为 IPv4 的协议处理器
    server_ip.register_handler(6, &server_tcp);  // TCP = 6
    client_ip.register_handler(6, &client_tcp);

    // --- 2. Server 监听 ---
    bool server_got_hello = false;

    server_tcp.listen(8080, [&](IStreamConnection* conn) -> StreamCallbacks {
        return {
            .on_receive = [&](IStreamConnection* c, const uint8_t* data, size_t len) {
                std::string msg(reinterpret_cast<const char*>(data), len);
                if (msg == "Hello") {
                    server_got_hello = true;
                    const uint8_t reply[] = "World";
                    c->send(reply, 5);
                }
            },
            .on_close = [](IStreamConnection*) {}
        };
    });

    // --- 3. Client 连接 ---
    bool client_connected = false;
    bool client_got_world = false;

    client_tcp.connect(server_addr, 8080,
        // on_connect
        [&](IStreamConnection* conn, int err) {
            if (err == 0) {
                client_connected = true;
                const uint8_t hello[] = "Hello";
                conn->send(hello, 5);
            }
        },
        // on_receive
        [&](IStreamConnection* conn, const uint8_t* data, size_t len) {
            std::string msg(reinterpret_cast<const char*>(data), len);
            if (msg == "World") {
                client_got_world = true;
                conn->close();
            }
        },
        // on_close
        [](IStreamConnection*) {}
    );

    // --- 4. 事件驱动循环 ---
    for (int tick = 0; tick < 200; ++tick) {
        // Client → Server
        transfer_packets(client_dev, server_ip);
        // Server → Client
        transfer_packets(server_dev, client_ip);

        // 定时器
        server_tcp.on_timer();
        client_tcp.on_timer();

        if (client_got_world) break;
    }

    // --- 5. 验证 ---
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (cond) {
            std::printf("  PASS: %s\n", msg);
        } else {
            std::printf("  FAIL: %s\n", msg);
            failures++;
        }
    };

    check(client_connected, "Client connected to server");
    check(server_got_hello, "Server received 'Hello'");
    check(client_got_world, "Client received 'World' echo");

    std::printf("\n%s\n", failures == 0
        ? "=== ALL PASSED ==="
        : "=== SOME TESTS FAILED ===");

    return failures;
}
