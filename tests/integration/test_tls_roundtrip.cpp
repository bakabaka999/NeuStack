/**
 * 集成测试：HTTPS over TLS 往返
 *
 * 使用 MockDevice 将两端的 IPv4 + TCP 层在内存中短接，
 * 验证 TLSLayer + HttpServer + HttpClient 的完整链路。
 */

#include "neustack/app/http_client.hpp"
#include "neustack/app/http_server.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"
#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/tls/tls_layer.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "../helpers/tls_test_utils.hpp"

#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <utility>
#include <vector>

using namespace neustack;
using namespace neustack::test_support;

class MockDevice : public NetDevice {
public:
    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock0"; }

    ssize_t send(const uint8_t *data, size_t len) override {
        _outbox.push(std::vector<uint8_t>(data, data + len));
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t *buf, size_t len, int) override {
        if (_inbox.empty()) {
            return 0;
        }

        auto &pkt = _inbox.front();
        const size_t n = std::min(len, pkt.size());
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

static void transfer_packets(MockDevice &from, IPv4Layer &to_ip) {
    while (from.has_output()) {
        auto pkt = from.pop_output();
        to_ip.on_receive(pkt.data(), pkt.size());
    }
}

int main() {
    Logger::instance().set_level(LogLevel::WARN);
    std::printf("=== TLS Roundtrip Integration Test ===\n");

    ScopedTLSFiles files;

    MockDevice server_dev, client_dev;
    IPv4Layer server_ip(server_dev);
    IPv4Layer client_ip(client_dev);

    const uint32_t server_addr = ip_from_string("192.168.100.1");
    const uint32_t client_addr = ip_from_string("192.168.100.2");

    server_ip.set_local_ip(server_addr);
    client_ip.set_local_ip(client_addr);

    TCPLayer server_tcp(server_ip, server_addr);
    TCPLayer client_tcp(client_ip, client_addr);

    // TLS handshake involves many small writes; disable Nagle to avoid stalls
    server_tcp.set_default_options(TCPOptions::low_latency());
    client_tcp.set_default_options(TCPOptions::low_latency());

    server_ip.register_handler(6, &server_tcp);
    client_ip.register_handler(6, &client_tcp);

    TLSLayer server_tls(server_tcp, server_tcp,
                        TLSContext::create_server(files.cert_path(), files.key_path()));
    TLSLayer client_tls(client_tcp, client_tcp,
                        nullptr,
                        TLSContext::create_client());

    HttpServer https_server(server_tls);
    HttpClient https_client(client_tls);

    bool handler_called = false;
    bool callback_called = false;
    bool request_success = false;
    std::string response_body;

    https_server.get("/secure", [&](const HttpRequest &) {
        handler_called = true;
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"message":"Hello from HTTPS"})");
    });

    https_server.listen(443);

    https_client.get(server_addr, 443, "/secure",
        [&](const HttpResponse &resp, int err) {
            callback_called = true;
            if (err != 0) {
                return;
            }

            if (resp.status == HttpStatus::OK &&
                resp.body.find("Hello from HTTPS") != std::string::npos) {
                response_body = resp.body;
                request_success = true;
            }
        });

    for (int tick = 0; tick < 600; ++tick) {
        // Multiple transfer rounds per tick to allow multi-segment exchanges
        for (int sub = 0; sub < 16; ++sub) {
            transfer_packets(client_dev, server_ip);
            transfer_packets(server_dev, client_ip);
            server_tcp.on_timer();
            client_tcp.on_timer();
        }

        https_server.poll();

        if (request_success) {
            break;
        }
    }

    int failures = 0;
    const auto check = [&](bool cond, const char *msg) {
        if (cond) {
            std::printf("  PASS: %s\n", msg);
        } else {
            std::printf("  FAIL: %s\n", msg);
            failures++;
        }
    };

    check(handler_called, "HTTPS handler was called");
    check(callback_called, "HTTPS client callback was called");
    check(request_success, "HTTPS response body contains expected data");
    check(response_body == R"({"message":"Hello from HTTPS"})",
          "HTTPS response body matches exactly");

    std::printf("\n%s\n", failures == 0
        ? "=== ALL PASSED ==="
        : "=== SOME TESTS FAILED ===");

    return failures;
}
