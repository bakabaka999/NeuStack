/**
 * 集成测试：TCP 连接压力测试
 *
 * 目标：验证 MAX_CONNECTIONS 限制和连接管理的稳定性。
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
// MockDevice — 内存网络设备 (复用自 test_tcp_handshake.cpp)
// ============================================================================

class MockDevice : public NetDevice {
public:
    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock_stress"; }

    ssize_t send(const uint8_t* data, size_t len) override {
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

    bool has_output() const { return !_outbox.empty(); }

    std::vector<uint8_t> pop_output() {
        auto pkt = std::move(_outbox.front());
        _outbox.pop();
        return pkt;
    }

    void inject(const std::vector<uint8_t>& pkt) {
        _inbox.push(pkt);
    }

private:
    std::queue<std::vector<uint8_t>> _outbox;
    std::queue<std::vector<uint8_t>> _inbox;
};

// ============================================================================
// 测试辅助函数
// ============================================================================

static void transfer_packets(MockDevice& from, IPv4Layer& to_ip) {
    int limit = 100; // 防止无限循环
    while (from.has_output() && limit-- > 0) {
        auto pkt = from.pop_output();
        to_ip.on_receive(pkt.data(), pkt.size());
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // 降低日志级别，否则 10000 次连接会产生海量日志
    Logger::instance().set_level(LogLevel::ERROR);

    std::printf("=== TCP Stress Integration Test ===\n");
    int failures = 0;

    auto check = [&](bool cond, const char* msg) {
        if (cond) {
            std::printf("  PASS: %s\n", msg);
        } else {
            std::printf("  FAIL: %s\n", msg);
            failures++;
        }
    };

    // ------------------------------------------------------------------------
    // 测试 1: MAX_CONNECTIONS 限制 (主动连接侧)
    // ------------------------------------------------------------------------
    {
        std::printf("\n[Test 1] MAX_CONNECTIONS (Limit: 10000)\n");

        MockDevice dev;
        IPv4Layer ip(dev);
        ip.set_local_ip(ip_from_string("192.168.1.1"));
        TCPLayer tcp(ip, ip_from_string("192.168.1.1"));
        ip.register_handler(6, &tcp);

        uint32_t remote_ip = ip_from_string("10.0.0.1");
        int success_count = 0;
        int fail_count = 0;

        // 尝试发起 10001 个连接
        // 我们不需要驱动数据包交换，connect() 调用时就会创建 TCB 并占用名额
        for (int i = 1; i <= 10001; ++i) {
            uint16_t remote_port = static_cast<uint16_t>(i); // 确保四元组唯一
            
            // local_port = 0 (自动分配)
            int ret = tcp.connect(remote_ip, remote_port, 0,
                [](IStreamConnection*, int) {}, 
                [](IStreamConnection*, const uint8_t*, size_t) {},
                [](IStreamConnection*) {}
            );

            if (ret == 0) {
                success_count++;
            } else {
                fail_count++;
            }
        }

        std::printf("  Stats: Success=%d, Failed=%d\n", success_count, fail_count);
        
        check(success_count == 10000, "Should accept exactly 10000 connections");
        check(fail_count == 1, "Should reject the 10001st connection");
    }

    // ------------------------------------------------------------------------
    // 测试 2: 完整连接生命周期 (基线检查)
    // ------------------------------------------------------------------------
    {
        std::printf("\n[Test 2] Baseline Connection Lifecycle\n");

        // Server Stack
        MockDevice server_dev;
        IPv4Layer server_ip(server_dev);
        uint32_t srv_addr = ip_from_string("192.168.100.1");
        server_ip.set_local_ip(srv_addr);
        TCPLayer server_tcp(server_ip, srv_addr);
        server_ip.register_handler(6, &server_tcp);

        // Client Stack
        MockDevice client_dev;
        IPv4Layer client_ip(client_dev);
        uint32_t cli_addr = ip_from_string("192.168.100.2");
        client_ip.set_local_ip(cli_addr);
        TCPLayer client_tcp(client_ip, cli_addr);
        client_ip.register_handler(6, &client_tcp);

        bool connected = false;
        bool server_got_close = false;

        // Server Listen
        server_tcp.listen(8080, [&](IStreamConnection* conn) -> StreamCallbacks {
            return {
                .on_receive = [](IStreamConnection*, const uint8_t*, size_t) {},
                .on_close = [&](IStreamConnection* c) {
                    server_got_close = true;
                    c->close();  // 服务端收到 FIN 后也关闭，完成四次挥手
                }
            };
        });

        // Client Connect
        client_tcp.connect(srv_addr, 8080, 0,
            [&](IStreamConnection* conn, int err) {
                if (err == 0) {
                    connected = true;
                    // 连接成功后立即关闭
                    conn->close();
                }
            },
            [](IStreamConnection*, const uint8_t*, size_t) {},
            [](IStreamConnection*) {}
        );

        // Drive Loop
        for (int i = 0; i < 200; ++i) {
            transfer_packets(client_dev, server_ip);
            transfer_packets(server_dev, client_ip);

            server_tcp.on_timer();
            client_tcp.on_timer();

            if (connected && server_got_close) break;
        }

        check(connected, "Client connected successfully");
        check(server_got_close, "Server received FIN and closed gracefully");
    }

    std::printf("\n%s\n", failures == 0 ? "=== ALL PASSED ===" : "=== SOME TESTS FAILED ===");
    return failures;
}