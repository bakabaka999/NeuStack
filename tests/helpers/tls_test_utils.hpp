#pragma once

#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/tls/tls_connection.hpp"
#include "neustack/transport/stream.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neustack::test_support {

inline constexpr char kServerCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDXzCCAkegAwIBAgIUArSv+WY6aaUBtQ2VzyadxB/U7aYwDQYJKoZIhvcNAQEL
BQAwPzEdMBsGA1UEAwwUTmV1U3RhY2sgVGVzdCBTZXJ2ZXIxETAPBgNVBAoMCE5l
dVN0YWNrMQswCQYDVQQGEwJVUzAeFw0yNjA0MDIwODQwMTFaFw0zNjAzMzAwODQw
MTFaMD8xHTAbBgNVBAMMFE5ldVN0YWNrIFRlc3QgU2VydmVyMREwDwYDVQQKDAhO
ZXVTdGFjazELMAkGA1UEBhMCVVMwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK
AoIBAQCTMInvxmY23KTAwLXGYXTaxNd/icJEe91e402w2k71D11Y3ArZp1G1hRxP
LkCvzKEP/thucxcwyjr14Tx9Id07Sw49xcRsyAMdiRb/qQJ2hr7jxvDF5yCNYw/v
z5kkD0qbUXIvrWti+eNzUET6IyuaRrcyeZ3brSH0DHU2IhAKc9/qGAXumx0kqbbK
nyxncmHCeJd7em2TucVcvGRgex7cEYXjKibpACIavlPsqJYkP8pLkI1fcAHhq9c2
uoSfZlR9dA4aKLPLZG3+ipHjJ6UhKDk13wl0RpLhyAZk8S5K8MI8lvFgc6eQnQ5K
KuknvNz6r64vUNABCSTtUFYk7kj9AgMBAAGjUzBRMB0GA1UdDgQWBBS1cp2O4LIv
6XfN0cYyeKOf2rHl2zAfBgNVHSMEGDAWgBS1cp2O4LIv6XfN0cYyeKOf2rHl2zAP
BgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAwXOEh5TPxbY7tk/V9
Q8iiT8OnNv7NuEnusT5xCDom0AGiclYJ1agqQZIhLlJo7A8EUdj1eMPbFam5+yzH
GjVR6wVaXfWQb3OLRzkz9/GCjHFtceTcIZQ4Paw8AC38vFQg6c8mXLxE0fQDZD31
ZwA/t7rJO1jbTLHcX8f77hdva0CilPIGjJKFMQOFdeZoeSYUscSFPMyzx33bx8w5
Qm5d5oK+YF8uyCutLlqxV+XV8Df4nSjjFHUsnuKPiAgfUVfMdltb3ebzyOMshEcv
RO2CTuetAH1fzCheKMkLA1Kavif5F//ihIUKYz0LZZC5VaxNtMmONtnCUykZVHIU
HIMe
-----END CERTIFICATE-----
)";

inline constexpr char kServerKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCTMInvxmY23KTA
wLXGYXTaxNd/icJEe91e402w2k71D11Y3ArZp1G1hRxPLkCvzKEP/thucxcwyjr1
4Tx9Id07Sw49xcRsyAMdiRb/qQJ2hr7jxvDF5yCNYw/vz5kkD0qbUXIvrWti+eNz
UET6IyuaRrcyeZ3brSH0DHU2IhAKc9/qGAXumx0kqbbKnyxncmHCeJd7em2TucVc
vGRgex7cEYXjKibpACIavlPsqJYkP8pLkI1fcAHhq9c2uoSfZlR9dA4aKLPLZG3+
ipHjJ6UhKDk13wl0RpLhyAZk8S5K8MI8lvFgc6eQnQ5KKuknvNz6r64vUNABCSTt
UFYk7kj9AgMBAAECggEAFo+F0QIGyEl1GlNfeVcWR3MM+fBGXeNjeMgb8CviFtdi
Tjy6EAcAs/NBddSH1zplBJPiHjnXV+Pjei0qLZ7rb0tvlSHTW/4jVhtdclX4Oe0o
5LZpUNg+qdVBi9c06K1MWPa4qxyofV0Ckzn+PJ338Ld+nIa+Hr4QmnzvWVsKvOMu
63Lg9//cyjsAaEOrstY6gs83lJ6FQPsNQvKxW3dzUtRU8f+Bz0vHpj401FOol9E6
aE4f3SdueZY8V6E69104auj0c10HZyddLbu7vSygOust1gzHornW2ybuPb+HKNjb
1HMAl0118i1jsKnyQ5ocNZptgqp1YwgJ7sGsduoaiQKBgQDHaMTfyEXkOU5RvQ5h
DlRIhuToFyS1nl+9SEpZPYHUEXkqtQnX4mobJXqpRSIWiHOshwkFlxP63eMh/ttW
VMs/tH2sXtkYSMLOegmSqzRvzSiHdrchsCzHnImb/bwSHwdvEiMTe0RxitQY3mW4
yZruqh6QBYm/Erk/IvlNQRTfFQKBgQC89fkrzDXrdnHpzwSQ2QvRMwVeZJ50gb0v
sH7nze4QrD0qPBy1H1RSm9eduFbWeUbJlaZHnIs1gTbZHenu02cd+03mU6Vrm28S
eFs3c+6CQkUouPQqrQgJFXPl4NUmWxiJf2j6OFbxEf1vXSN6TCtGnrzVen6oDIhP
EqTIaVr8SQKBgCIsKyjqakfNJkXNr0wkp37yVwILDUhuhpuqastWRgxwniIaekBb
1bEnQBkH9uqDocccMQibNlpUchseULo/t8EIDk1ex7doqLG3qjJPUqIiN2LXjlSg
m1vt0ItB9Vvpo11+bzJkens2vlgwrJ/5NKrXznsB5/Qtoj9WoACEOa5tAoGAUjme
0aYtGGcULq+8xuMLt17CSU7zfLMwBd6BepErtOmePBCoVVBquv/BlYovj2h6myGZ
l7PRB9lQaq4Pq7MmPe+q+D0R0H90l87zsm+qK9h2i7/fz+o3guxo4HEzj1s4lCxz
G98ERiaT56/Zzk/yzOoqNaL2Fl5NukrclbDyvLECgYEAnjcxoN1SsvoYK86XkI7u
wbW4nnyh4bd3ZVrBPPB9Jn6oVI9ksENHNCtGw4HVyGMepzXgtm7IPeqnmNCTnFVo
ZXXp12dMmBe8wPJQvGhdI4JsecHMbsV/LEr72Og8Oa9cLeTjkVmqfL0rg6z0IVV4
+4nqH7L5DFB94sG7hKpP+eo=
-----END PRIVATE KEY-----
)";

inline constexpr char kOtherCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDWTCCAkGgAwIBAgIUUVbFC42WzjvmCOPOnSnis5EKSKswDQYJKoZIhvcNAQEL
BQAwPDEaMBgGA1UEAwwRTmV1U3RhY2sgT3RoZXIgQ0ExETAPBgNVBAoMCE5ldVN0
YWNrMQswCQYDVQQGEwJVUzAeFw0yNjA0MDIwODQwMTFaFw0zNjAzMzAwODQwMTFa
MDwxGjAYBgNVBAMMEU5ldVN0YWNrIE90aGVyIENBMREwDwYDVQQKDAhOZXVTdGFj
azELMAkGA1UEBhMCVVMwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCs
Aex5xKJBWoT3/PnCeIi0ZvXfUhURANHkwJm931TNc4qwpEeCNoR7h/i3iEvrmy9X
/f7WjaYii0BVa34p7+Kh3/8OwWMMqgxZZIwC+/hmzSR1687/bjqDDQGvyBkNgZzI
xdN/DkQalayyl+VgtobvOd572EtkPcRwVxxfL0muILYVgMfzKRXNTIANIOwOby6u
8ehQWr2taBmh2t+cvehu2+d6YG2eHg4qNLjyQU7MEgZpGE8f2tFaIFwVI98+sUu5
2kTL8Xx9+YTSviWNc75b2qe8eYxW1JIF9WGDth3SeArAUaHPdjdS4tTu6ceOUI+w
7g5f/1NK/FgEelIq0VItAgMBAAGjUzBRMB0GA1UdDgQWBBQCwZaiVfCLQVXY53c6
MiL/ic3TUTAfBgNVHSMEGDAWgBQCwZaiVfCLQVXY53c6MiL/ic3TUTAPBgNVHRMB
Af8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQA9X4lFoHCCIIqSCjpF37X5gYaM
1vuSao+KW6OgxhZrHqA6LPaM4qd9eyP6nv8fSyIDQ7CiqTZ8DiFXNFZhr9nUVmNy
BDwUbnDOiTOBb2c3/x1R02lHRMkJWqRNCSnXq9aCNajU8nCH7MA0tdjiidm7rkAe
g8V59e0eH3j4RMR3SQTeEy0V5YKs319/EpuSlubPyDS9tn4kRHLVQfyagUV5229L
vwRU9yUSiSqPkG1FFIqHwcSUCocn98MTs7PSO/AkH2cpq+l8PqCoXcR59n6+rI5y
1w5fQLsD1d18QVa/Mn8aRNyK7df7tfoJmMsQNaddt7fCxfF4EH67uMjxeVBi
-----END CERTIFICATE-----
)";

class ScopedTLSFiles {
public:
    ScopedTLSFiles() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        _dir = std::filesystem::temp_directory_path() /
               ("neustack_tls_test_" + std::to_string(nonce));
        std::filesystem::create_directories(_dir);

        _cert_path = (_dir / "server_cert.pem").string();
        _key_path = (_dir / "server_key.pem").string();
        _other_ca_path = (_dir / "other_ca.pem").string();

        write_file(_cert_path, kServerCertPem);
        write_file(_key_path, kServerKeyPem);
        write_file(_other_ca_path, kOtherCertPem);
    }

    ~ScopedTLSFiles() {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    const std::string &cert_path() const { return _cert_path; }
    const std::string &key_path() const { return _key_path; }
    const std::string &other_ca_path() const { return _other_ca_path; }

private:
    static void write_file(const std::string &path, const char *contents) {
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }

    std::filesystem::path _dir;
    std::string _cert_path;
    std::string _key_path;
    std::string _other_ca_path;
};

class MemoryStreamConnection : public IStreamConnection {
public:
    MemoryStreamConnection(uint32_t remote_ip = 0, uint16_t remote_port = 0)
        : _remote_ip(remote_ip), _remote_port(remote_port) {}

    ssize_t send(const uint8_t *data, size_t len) override {
        _outbound.insert(_outbound.end(), data, data + len);
        return static_cast<ssize_t>(len);
    }

    void close() override {
        if (_closed) {
            return;
        }
        _closed = true;
        if (_close_hook) {
            _close_hook(this);
        }
    }

    uint32_t remote_ip() const override { return _remote_ip; }
    uint16_t remote_port() const override { return _remote_port; }
    StreamError last_error() const override { return _last_error; }
    uint8_t last_error_detail() const override { return _last_error_detail; }

    bool closed() const { return _closed; }
    bool has_outbound() const { return !_outbound.empty(); }

    std::vector<uint8_t> take_outbound() {
        auto data = std::move(_outbound);
        _outbound.clear();
        return data;
    }

    void set_close_hook(std::function<void(MemoryStreamConnection *)> hook) {
        _close_hook = std::move(hook);
    }

    void set_error(StreamError error, uint8_t detail = 0) {
        _last_error = error;
        _last_error_detail = detail;
    }

private:
    uint32_t _remote_ip = 0;
    uint16_t _remote_port = 0;
    StreamError _last_error = StreamError::None;
    uint8_t _last_error_detail = 0;
    bool _closed = false;
    std::vector<uint8_t> _outbound;
    std::function<void(MemoryStreamConnection *)> _close_hook;
};

inline void transfer_ciphertext(MemoryStreamConnection &from, TLSStreamConnection &to) {
    auto data = from.take_outbound();
    if (!data.empty()) {
        to.on_tcp_data(data.data(), data.size());
    }
}

inline bool pump_tls_handshake(TLSStreamConnection &client_tls,
                               MemoryStreamConnection &client_raw,
                               TLSStreamConnection &server_tls,
                               MemoryStreamConnection &server_raw,
                               int max_ticks = 64) {
    for (int tick = 0; tick < max_ticks; ++tick) {
        if (client_tls.handshake_state() == TLSHandshakeState::Complete &&
            server_tls.handshake_state() == TLSHandshakeState::Complete) {
            return true;
        }
        if (client_tls.handshake_state() == TLSHandshakeState::Failed ||
            server_tls.handshake_state() == TLSHandshakeState::Failed) {
            return false;
        }

        client_tls.drive_handshake();
        transfer_ciphertext(client_raw, server_tls);

        server_tls.drive_handshake();
        transfer_ciphertext(server_raw, client_tls);
    }

    return client_tls.handshake_state() == TLSHandshakeState::Complete &&
           server_tls.handshake_state() == TLSHandshakeState::Complete;
}

class MemoryStreamTransport : public IStreamServer, public IStreamClient {
public:
    int listen(uint16_t port, StreamAcceptCallback on_accept) override {
        _port = port;
        _accept = std::move(on_accept);
        return 0;
    }

    void unlisten(uint16_t port) override {
        if (_port == port) {
            _port = 0;
            _accept = {};
        }
    }

    int connect(uint32_t remote_ip, uint16_t remote_port,
                ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close) override {
        if (!_accept || remote_port != _port) {
            on_connect(nullptr, -1);
            return -1;
        }

        _server_conn = std::make_unique<MemoryStreamConnection>(remote_ip, remote_port);
        _client_conn = std::make_unique<MemoryStreamConnection>(remote_ip, remote_port);

        _server_conn->set_close_hook([this](MemoryStreamConnection *) { request_close(); });
        _client_conn->set_close_hook([this](MemoryStreamConnection *) { request_close(); });

        _server_callbacks = _accept(_server_conn.get());
        _client_callbacks = {std::move(on_receive), std::move(on_close)};
        _client_on_connect = std::move(on_connect);

        _client_on_connect(_client_conn.get(), 0);
        return 0;
    }

    bool pump_once() {
        bool progressed = false;

        if (_client_conn && _server_callbacks.on_receive && _client_conn->has_outbound()) {
            auto payload = _client_conn->take_outbound();
            _server_callbacks.on_receive(_server_conn.get(), payload.data(), payload.size());
            progressed = true;
        }

        if (_server_conn && _client_callbacks.on_receive && _server_conn->has_outbound()) {
            auto payload = _server_conn->take_outbound();
            _client_callbacks.on_receive(_client_conn.get(), payload.data(), payload.size());
            progressed = true;
        }

        return progressed;
    }

    bool pump_until_idle(int max_ticks = 64) {
        bool progressed = false;
        for (int tick = 0; tick < max_ticks; ++tick) {
            if (!pump_once()) {
                break;
            }
            progressed = true;
        }
        return progressed;
    }

private:
    void request_close() {
        if (_closing) {
            return;
        }

        _closing = true;

        if (!_server_close_notified && _server_callbacks.on_close && _server_conn) {
            _server_close_notified = true;
            _server_callbacks.on_close(_server_conn.get());
        }

        if (!_client_close_notified && _client_callbacks.on_close && _client_conn) {
            _client_close_notified = true;
            _client_callbacks.on_close(_client_conn.get());
        }

        _closing = false;
    }

    uint16_t _port = 0;
    bool _closing = false;
    bool _server_close_notified = false;
    bool _client_close_notified = false;

    StreamAcceptCallback _accept;
    ConnectCallback _client_on_connect;
    StreamCallbacks _server_callbacks;
    StreamCallbacks _client_callbacks;
    std::unique_ptr<MemoryStreamConnection> _server_conn;
    std::unique_ptr<MemoryStreamConnection> _client_conn;
};

} // namespace neustack::test_support

#endif // NEUSTACK_TLS_ENABLED
