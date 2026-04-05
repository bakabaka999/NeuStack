#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/tls/tls_connection.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <algorithm>

namespace neustack {

// ─── BIO 回调 ───

int TLSStreamConnection::bio_send(void *ctx, const unsigned char *buf, size_t len) {
    auto *self = static_cast<TLSStreamConnection *>(ctx);
    ssize_t sent = self->_inner->send(buf, len);
    if (sent <= 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return static_cast<int>(sent);
}

int TLSStreamConnection::bio_recv(void *ctx, unsigned char *buf, size_t len) {
    auto *self = static_cast<TLSStreamConnection *>(ctx);

    size_t available = self->_recv_buf.size() - self->_recv_offset;
    if (available == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t to_copy = std::min(len, available);
    std::memcpy(buf, self->_recv_buf.data() + self->_recv_offset, to_copy);
    self->_recv_offset += to_copy;

    // 回收已消费的缓冲区
    if (self->_recv_offset == self->_recv_buf.size()) {
        self->_recv_buf.clear();
        self->_recv_offset = 0;
    }

    return static_cast<int>(to_copy);
}

// ─── 构造 / 析构 ───

TLSStreamConnection::TLSStreamConnection(IStreamConnection *inner, TLSContext &ctx)
    : _inner(inner), _ctx(ctx) {

    mbedtls_ssl_init(&_ssl);

    int ret = mbedtls_ssl_setup(&_ssl, &_ctx.ssl_config());
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "ssl_setup failed: %s (0x%04x)", errbuf, -ret);
        _hs_state = TLSHandshakeState::Failed;
        return;
    }

    // 设置 BIO 回调
    mbedtls_ssl_set_bio(&_ssl, this, bio_send, bio_recv, nullptr);

    _hs_state = TLSHandshakeState::Pending;
}

TLSStreamConnection::~TLSStreamConnection() {
    mbedtls_ssl_free(&_ssl);
}

// ─── IStreamConnection 接口 ───

ssize_t TLSStreamConnection::send(const uint8_t *data, size_t len) {
    if (_hs_state != TLSHandshakeState::Complete) {
        return -1;  // 握手未完成，不允许发送
    }

    int ret = mbedtls_ssl_write(&_ssl, data, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
        return 0;  // 非阻塞：暂时无法发送
    }
    if (ret < 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN(TLS, "ssl_write failed: %s (0x%04x)", errbuf, -ret);
        _last_error = StreamError::TLSAlert;
        return -1;
    }
    return ret;
}

void TLSStreamConnection::close() {
    if (_hs_state == TLSHandshakeState::Complete) {
        // Best-effort close_notify before closing TCP.
        // Ignore WANT_WRITE / errors — peer may have closed already.
        _hs_state = TLSHandshakeState::Pending;  // prevent re-entrant TLS I/O
        mbedtls_ssl_close_notify(&_ssl);
    } else {
        _hs_state = TLSHandshakeState::Pending;
    }
    _inner->close();
}

uint32_t TLSStreamConnection::remote_ip() const {
    return _inner->remote_ip();
}

uint16_t TLSStreamConnection::remote_port() const {
    return _inner->remote_port();
}

StreamError TLSStreamConnection::last_error() const {
    return _last_error;
}

uint8_t TLSStreamConnection::last_error_detail() const {
    return _inner->last_error_detail();
}

// ─── TLS 特有 API ───

void TLSStreamConnection::on_tcp_data(const uint8_t *data, size_t len) {
    _recv_buf.insert(_recv_buf.end(), data, data + len);
}

void TLSStreamConnection::set_hostname(const std::string &hostname) {
    int ret = mbedtls_ssl_set_hostname(&_ssl, hostname.c_str());
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN(TLS, "ssl_set_hostname(%s) failed: %s", hostname.c_str(), errbuf);
    }
}

TLSHandshakeState TLSStreamConnection::drive_handshake() {
    if (_hs_state == TLSHandshakeState::Complete ||
        _hs_state == TLSHandshakeState::Failed) {
        return _hs_state;
    }

    _hs_state = TLSHandshakeState::InProgress;

    int ret = mbedtls_ssl_handshake(&_ssl);

    if (ret == 0) {
        _hs_state = TLSHandshakeState::Complete;
        LOG_INFO(TLS, "TLS handshake complete (cipher=%s)",
                 mbedtls_ssl_get_ciphersuite(&_ssl));
    } else if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
               ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        // 正常：等待更多数据
        _hs_state = TLSHandshakeState::InProgress;
    } else {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "TLS handshake failed: %s (0x%04x)", errbuf, -ret);
        _hs_state = TLSHandshakeState::Failed;
        _last_error = StreamError::TLSHandshakeFailed;
    }

    return _hs_state;
}

int TLSStreamConnection::read_decrypted(uint8_t *buf, size_t max_len) {
    if (_hs_state != TLSHandshakeState::Complete) {
        return 0;
    }

    int ret = mbedtls_ssl_read(&_ssl, buf, max_len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;  // 对端关闭
    }
    if (ret < 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN(TLS, "ssl_read failed: %s (0x%04x)", errbuf, -ret);
        _last_error = StreamError::TLSAlert;
        return -1;
    }
    return ret;
}

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
