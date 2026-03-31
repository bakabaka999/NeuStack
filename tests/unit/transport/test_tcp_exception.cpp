#include <catch2/catch_test_macros.hpp>

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <chrono>
#include <cstring>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace neustack;

namespace {

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

    ssize_t recv(uint8_t* buf, size_t len, int /*timeout_ms*/) override {
        if (_inbox.empty()) return 0;
        auto& pkt = _inbox.front();
        size_t n = std::min(len, pkt.size());
        std::memcpy(buf, pkt.data(), n);
        _inbox.pop();
        return static_cast<ssize_t>(n);
    }

private:
    std::queue<std::vector<uint8_t>> _outbox;
    std::queue<std::vector<uint8_t>> _inbox;
};

} // namespace

TEST_CASE("TCP timeout path reports connect failure and telemetry", "[transport][tcp][exception]") {
    Logger::instance().set_level(LogLevel::WARN);

    MockDevice client_dev;
    IPv4Layer client_ip(client_dev);
    uint32_t client_addr = ip_from_string("10.0.0.2");
    uint32_t remote_addr = ip_from_string("10.0.0.1");
    client_ip.set_local_ip(client_addr);

    TCPLayer client_tcp(client_ip, client_addr);
    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &client_tcp);

    TCPOptions opts = TCPOptions::low_latency();
    opts.initial_rto_us = 1000;
    opts.max_retransmit = 1;
    client_tcp.set_default_options(opts);

    auto &gm = global_metrics();
    uint64_t retrans_before = gm.total_retransmits.load(std::memory_order_relaxed);
    uint64_t timeout_before = gm.conn_timeout.load(std::memory_order_relaxed);

    bool callback_called = false;
    int callback_error = 0;

    REQUIRE(client_tcp.connect(
        remote_addr, 80,
        [&](IStreamConnection* conn, int err) {
            callback_called = true;
            callback_error = err;
            CHECK(conn == nullptr);
        },
        [](IStreamConnection*, const uint8_t*, size_t) {},
        [](IStreamConnection*) {}) == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    client_tcp.on_timer();

    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    client_tcp.on_timer();

    REQUIRE(callback_called);
    CHECK(callback_error == -1);
    CHECK(client_tcp.connection_manager().connection_count() == 0);
    CHECK(gm.total_retransmits.load(std::memory_order_relaxed) - retrans_before == 1);
    CHECK(gm.conn_timeout.load(std::memory_order_relaxed) - timeout_before == 1);
}
