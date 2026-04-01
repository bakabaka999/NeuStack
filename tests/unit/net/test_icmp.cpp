#include <catch2/catch_test_macros.hpp>

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <queue>
#include <string>
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

} // namespace

TEST_CASE("ICMP error propagation reaches UDP socket callbacks", "[net][icmp]") {
    Logger::instance().set_level(LogLevel::WARN);

    MockDevice client_dev;
    MockDevice remote_dev;

    IPv4Layer client_ip(client_dev);
    IPv4Layer remote_ip(remote_dev);

    uint32_t client_addr = ip_from_string("10.0.0.2");
    uint32_t remote_addr = ip_from_string("10.0.0.1");
    client_ip.set_local_ip(client_addr);
    remote_ip.set_local_ip(remote_addr);

    ICMPHandler client_icmp(client_ip);
    ICMPHandler remote_icmp(remote_ip);
    UDPLayer client_udp(client_ip);

    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &client_icmp);
    client_icmp.set_error_callback(static_cast<uint8_t>(IPProtocol::UDP),
                                   [&client_udp](const ICMPErrorInfo &error) {
                                       client_udp.handle_icmp_error(error);
                                   });

    uint16_t port = client_udp.bind(12345, [](uint32_t, uint16_t, const uint8_t*, size_t) {});
    REQUIRE(port == 12345);

    bool error_called = false;
    ICMPErrorInfo captured{};
    client_udp.set_error_callback(port, [&](const ICMPErrorInfo &error) {
        error_called = true;
        captured = error;
    });

    const uint8_t payload[] = {'d', 'n', 's'};
    REQUIRE(client_udp.sendto(remote_addr, 53, port, payload, sizeof(payload)) > 0);

    auto original = client_dev.pop_output();
    auto original_pkt = IPv4Parser::parse(original.data(), original.size());
    REQUIRE(original_pkt.has_value());

    remote_icmp.send_dest_unreachable(ICMPUnreachCode::PortUnreachable, *original_pkt);
    transfer_packets(remote_dev, client_ip);

    REQUIRE(error_called);
    CHECK(captured.type == ICMPType::DestUnreachable);
    CHECK(captured.code == static_cast<uint8_t>(ICMPUnreachCode::PortUnreachable));
    CHECK(captured.local_ip == client_addr);
    CHECK(captured.remote_ip == remote_addr);
    CHECK(captured.local_port == port);
    CHECK(captured.remote_port == 53);
    CHECK(captured.protocol == static_cast<uint8_t>(IPProtocol::UDP));
}

TEST_CASE("ICMP error propagation aborts TCP connect handshake", "[net][icmp]") {
    Logger::instance().set_level(LogLevel::WARN);

    MockDevice client_dev;
    MockDevice remote_dev;

    IPv4Layer client_ip(client_dev);
    IPv4Layer remote_ip(remote_dev);

    uint32_t client_addr = ip_from_string("10.0.0.2");
    uint32_t remote_addr = ip_from_string("10.0.0.1");
    client_ip.set_local_ip(client_addr);
    remote_ip.set_local_ip(remote_addr);

    ICMPHandler client_icmp(client_ip);
    ICMPHandler remote_icmp(remote_ip);
    TCPLayer client_tcp(client_ip, client_addr);

    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &client_icmp);
    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &client_tcp);
    client_icmp.set_error_callback(static_cast<uint8_t>(IPProtocol::TCP),
                                   [&client_tcp](const ICMPErrorInfo &error) {
                                       client_tcp.handle_icmp_error(error);
                                   });

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

    auto syn = client_dev.pop_output();
    auto syn_pkt = IPv4Parser::parse(syn.data(), syn.size());
    REQUIRE(syn_pkt.has_value());

    remote_icmp.send_dest_unreachable(ICMPUnreachCode::HostUnreachable, *syn_pkt);
    transfer_packets(remote_dev, client_ip);

    REQUIRE(callback_called);
    CHECK(callback_error == -1);
    CHECK(client_tcp.connection_manager().connection_count() == 0);
}

TEST_CASE("ICMP error propagation closes established TCP streams", "[net][icmp]") {
    Logger::instance().set_level(LogLevel::WARN);

    MockDevice client_dev;
    MockDevice server_dev;

    IPv4Layer client_ip(client_dev);
    IPv4Layer server_ip(server_dev);

    uint32_t client_addr = ip_from_string("10.0.0.2");
    uint32_t server_addr = ip_from_string("10.0.0.1");
    client_ip.set_local_ip(client_addr);
    server_ip.set_local_ip(server_addr);

    ICMPHandler client_icmp(client_ip);
    ICMPHandler server_icmp(server_ip);
    TCPLayer client_tcp(client_ip, client_addr);
    TCPLayer server_tcp(server_ip, server_addr);

    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &client_icmp);
    client_ip.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &client_tcp);
    server_ip.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &server_tcp);

    client_icmp.set_error_callback(static_cast<uint8_t>(IPProtocol::TCP),
                                   [&client_tcp](const ICMPErrorInfo &error) {
                                       client_tcp.handle_icmp_error(error);
                                   });

    bool client_connected = false;
    bool server_connected = false;
    bool close_called = false;
    StreamError close_error = StreamError::None;
    uint8_t close_detail = 0;
    IStreamConnection* client_conn = nullptr;

    server_tcp.listen(8080, [&](IStreamConnection*) -> StreamCallbacks {
        server_connected = true;
        return {
            .on_receive = [](IStreamConnection*, const uint8_t*, size_t) {},
            .on_close = [](IStreamConnection*) {}
        };
    });

    REQUIRE(client_tcp.connect(
        server_addr, 8080,
        [&](IStreamConnection* conn, int err) {
            REQUIRE(err == 0);
            REQUIRE(conn != nullptr);
            client_connected = true;
            client_conn = conn;
        },
        [](IStreamConnection*, const uint8_t*, size_t) {},
        [&](IStreamConnection* conn) {
            close_called = true;
            close_error = conn->last_error();
            close_detail = conn->last_error_detail();
        }) == 0);

    for (int tick = 0; tick < 200; ++tick) {
        transfer_packets(client_dev, server_ip);
        transfer_packets(server_dev, client_ip);
        server_tcp.on_timer();
        client_tcp.on_timer();

        if (client_connected && server_connected) {
            break;
        }
    }

    REQUIRE(client_connected);
    REQUIRE(server_connected);
    REQUIRE(client_conn != nullptr);

    const uint8_t payload[] = {'p', 'i', 'n', 'g'};

    SECTION("destination unreachable is surfaced to on_close consumers") {
        REQUIRE(client_conn->send(payload, sizeof(payload)) == 4);

        auto original = client_dev.pop_output();
        auto original_pkt = IPv4Parser::parse(original.data(), original.size());
        REQUIRE(original_pkt.has_value());

        server_icmp.send_dest_unreachable(ICMPUnreachCode::HostUnreachable, *original_pkt);
        transfer_packets(server_dev, client_ip);

        REQUIRE(close_called);
        CHECK(close_error == StreamError::ICMPUnreachable);
        CHECK(close_detail == static_cast<uint8_t>(ICMPUnreachCode::HostUnreachable));
        CHECK(client_tcp.connection_manager().connection_count() == 0);
    }

    SECTION("time exceeded is surfaced to on_close consumers") {
        REQUIRE(client_conn->send(payload, sizeof(payload)) == 4);

        auto original = client_dev.pop_output();
        auto original_pkt = IPv4Parser::parse(original.data(), original.size());
        REQUIRE(original_pkt.has_value());

        server_icmp.send_time_exceeded(ICMPTimeExCode::TTLExceeded, *original_pkt);
        transfer_packets(server_dev, client_ip);

        REQUIRE(close_called);
        CHECK(close_error == StreamError::ICMPTimeExceeded);
        CHECK(close_detail == static_cast<uint8_t>(ICMPTimeExCode::TTLExceeded));
        CHECK(client_tcp.connection_manager().connection_count() == 0);
    }
}
