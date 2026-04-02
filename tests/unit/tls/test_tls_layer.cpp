#include <catch2/catch_test_macros.hpp>

#include "neustack/tls/tls_layer.hpp"
#include "../../helpers/tls_test_utils.hpp"

#include <string>

using namespace neustack;
using namespace neustack::test_support;

TEST_CASE("TLSLayer delays app callbacks until handshake completes", "[tls][unit]") {
    ScopedTLSFiles files;
    MemoryStreamTransport transport;

    TLSLayer server_tls(transport, transport,
                        TLSContext::create_server(files.cert_path(), files.key_path()));
    TLSLayer client_tls(transport, transport,
                        nullptr,
                        TLSContext::create_client());

    bool server_accepted = false;
    bool client_connected = false;
    bool server_closed = false;
    bool client_closed = false;
    std::string server_payload;
    std::string client_payload;
    IStreamConnection *client_conn = nullptr;

    REQUIRE(server_tls.listen(443, [&](IStreamConnection *conn) -> StreamCallbacks {
        server_accepted = true;
        CHECK(conn != nullptr);

        return StreamCallbacks{
            [&](IStreamConnection *stream, const uint8_t *data, size_t len) {
                server_payload.assign(reinterpret_cast<const char *>(data), len);
                const std::string reply = "server-ack";
                REQUIRE(stream->send(reinterpret_cast<const uint8_t *>(reply.data()), reply.size()) ==
                        static_cast<ssize_t>(reply.size()));
            },
            [&](IStreamConnection *) { server_closed = true; }
        };
    }) == 0);

    REQUIRE(client_tls.connect(
        0xC0A86401, 443,
        [&](IStreamConnection *conn, int error) {
            REQUIRE(error == 0);
            REQUIRE(conn != nullptr);
            client_connected = true;
            client_conn = conn;

            const std::string request = "client-hello";
            REQUIRE(conn->send(reinterpret_cast<const uint8_t *>(request.data()), request.size()) ==
                    static_cast<ssize_t>(request.size()));
        },
        [&](IStreamConnection *, const uint8_t *data, size_t len) {
            client_payload.assign(reinterpret_cast<const char *>(data), len);
        },
        [&](IStreamConnection *) { client_closed = true; }) == 0);

    CHECK_FALSE(server_accepted);
    CHECK_FALSE(client_connected);

    for (int tick = 0; tick < 128; ++tick) {
        transport.pump_until_idle();
        if (server_accepted && client_connected && client_payload == "server-ack") {
            break;
        }
    }

    CHECK(server_accepted);
    CHECK(client_connected);
    CHECK(server_payload == "client-hello");
    CHECK(client_payload == "server-ack");

    REQUIRE(client_conn != nullptr);
    client_conn->close();
    transport.pump_until_idle();

    CHECK(server_closed);
    CHECK(client_closed);
}

TEST_CASE("TLSLayer client connect fails without client context", "[tls][unit]") {
    ScopedTLSFiles files;
    MemoryStreamTransport transport;

    TLSLayer server_tls(transport, transport,
                        TLSContext::create_server(files.cert_path(), files.key_path()));
    TLSLayer client_tls(transport, transport, nullptr, nullptr);

    REQUIRE(server_tls.listen(443, [](IStreamConnection *) -> StreamCallbacks {
        return {};
    }) == 0);

    CHECK(client_tls.connect(
              0xC0A86401, 443,
              [](IStreamConnection *, int) {},
              [](IStreamConnection *, const uint8_t *, size_t) {},
              [](IStreamConnection *) {}) == -1);
}
