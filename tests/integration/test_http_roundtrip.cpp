/**
 * 集成测试：HTTP 请求/响应往返 (Roundtrip)
 *
 * 使用 MockDevice 将两端的 IPv4 层在内存中短接，
 * 验证 HttpServer + HttpClient 完整流程。
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/app/http_server.hpp"
#include "neustack/app/http_client.hpp"
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
// MockDevice
// ============================================================================

class MockDevice : public NetDevice {
public:
    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock0"; }

    ssize_t send(const uint8_t* data, size_t len) override {
        _outbox.push(std::vector<uint8_t>(data, data + len));
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t* buf, size_t len, int) override {
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

private:
    std::queue<std::vector<uint8_t>> _outbox;
    std::queue<std::vector<uint8_t>> _inbox;
};

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
    Logger::instance().set_level(LogLevel::WARN);
    std::printf("=== HTTP Roundtrip Integration Test ===\n");

    // --- 1. 构建双端协议栈 ---
    MockDevice server_dev, client_dev;

    IPv4Layer server_ip(server_dev);
    IPv4Layer client_ip(client_dev);

    uint32_t server_addr = ip_from_string("192.168.100.1");
    uint32_t client_addr = ip_from_string("192.168.100.2");

    server_ip.set_local_ip(server_addr);
    client_ip.set_local_ip(client_addr);

    TCPLayer server_tcp(server_ip, server_addr);
    TCPLayer client_tcp(client_ip, client_addr);

    server_ip.register_handler(6, &server_tcp);
    client_ip.register_handler(6, &client_tcp);

    // --- 2. HTTP Server ---
    HttpServer http_server(server_tcp);

    bool handler_called = false;
    http_server.get("/api/data", [&](const HttpRequest&) {
        handler_called = true;
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"message":"Hello from Server"})");
    });

    http_server.get("/api/echo", [](const HttpRequest& req) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("path=" + req.path);
    });

    http_server.listen(80);

    // --- 3. HTTP Client 发起请求 ---
    HttpClient http_client(client_tcp);

    bool callback_called = false;
    bool request_success = false;
    std::string response_body;

    http_client.get(server_addr, 80, "/api/data",
        [&](const HttpResponse& resp, int err) {
            callback_called = true;
            if (err != 0) return;
            if (resp.status == HttpStatus::OK) {
                response_body = resp.body;
                if (resp.body.find("Hello from Server") != std::string::npos) {
                    request_success = true;
                }
            }
        }
    );

    // --- 4. 事件驱动循环 ---
    for (int tick = 0; tick < 500; ++tick) {
        transfer_packets(client_dev, server_ip);
        transfer_packets(server_dev, client_ip);

        server_tcp.on_timer();
        client_tcp.on_timer();

        http_server.poll();

        if (request_success) break;
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

    check(handler_called, "Server handler was called");
    check(callback_called, "Client callback was called");
    check(request_success, "Response body contains expected data");

    std::printf("\n%s\n", failures == 0
        ? "=== ALL PASSED ==="
        : "=== SOME TESTS FAILED ===");

    return failures;
}
