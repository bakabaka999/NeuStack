#include <catch2/catch_test_macros.hpp>

#include "neustack/tls/tls_connection.hpp"
#include "neustack/tls/tls_context.hpp"
#include "../../helpers/tls_test_utils.hpp"

#include <array>
#include <string>

using namespace neustack;
using namespace neustack::test_support;

TEST_CASE("TLSStreamConnection completes handshake and exchanges plaintext", "[tls][unit]") {
    ScopedTLSFiles files;

    auto server_ctx = TLSContext::create_server(files.cert_path(), files.key_path());
    auto client_ctx = TLSContext::create_client();

    REQUIRE(server_ctx);
    REQUIRE(client_ctx);

    MemoryStreamConnection server_raw(0xC0A86401, 443);
    MemoryStreamConnection client_raw(0xC0A86402, 443);

    TLSStreamConnection server_tls(&server_raw, *server_ctx);
    TLSStreamConnection client_tls(&client_raw, *client_ctx);

    REQUIRE(pump_tls_handshake(client_tls, client_raw, server_tls, server_raw));
    CHECK(client_tls.handshake_state() == TLSHandshakeState::Complete);
    CHECK(server_tls.handshake_state() == TLSHandshakeState::Complete);

    const std::string request = "hello over tls";
    REQUIRE(client_tls.send(reinterpret_cast<const uint8_t *>(request.data()), request.size()) ==
            static_cast<ssize_t>(request.size()));

    transfer_ciphertext(client_raw, server_tls);

    std::array<uint8_t, 128> plaintext{};
    const int server_read = server_tls.read_decrypted(plaintext.data(), plaintext.size());
    REQUIRE(server_read == static_cast<int>(request.size()));
    CHECK(std::string(reinterpret_cast<const char *>(plaintext.data()), server_read) == request);

    const std::string response = "secure reply";
    REQUIRE(server_tls.send(reinterpret_cast<const uint8_t *>(response.data()), response.size()) ==
            static_cast<ssize_t>(response.size()));

    transfer_ciphertext(server_raw, client_tls);

    const int client_read = client_tls.read_decrypted(plaintext.data(), plaintext.size());
    REQUIRE(client_read == static_cast<int>(response.size()));
    CHECK(std::string(reinterpret_cast<const char *>(plaintext.data()), client_read) == response);
}

TEST_CASE("TLSStreamConnection reports handshake failure for untrusted peer", "[tls][unit]") {
    ScopedTLSFiles files;

    auto server_ctx = TLSContext::create_server(files.cert_path(), files.key_path());
    auto client_ctx = TLSContext::create_client(files.other_ca_path());

    REQUIRE(server_ctx);
    REQUIRE(client_ctx);

    MemoryStreamConnection server_raw(0xC0A86401, 443);
    MemoryStreamConnection client_raw(0xC0A86402, 443);

    TLSStreamConnection server_tls(&server_raw, *server_ctx);
    TLSStreamConnection client_tls(&client_raw, *client_ctx);

    CHECK_FALSE(pump_tls_handshake(client_tls, client_raw, server_tls, server_raw, 128));
    CHECK((client_tls.handshake_state() == TLSHandshakeState::Failed ||
           server_tls.handshake_state() == TLSHandshakeState::Failed));
    CHECK((client_tls.last_error() == StreamError::TLSHandshakeFailed ||
           server_tls.last_error() == StreamError::TLSHandshakeFailed));
}
