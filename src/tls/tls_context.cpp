#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/tls/tls_context.hpp"
#include "neustack/common/log.hpp"

#include <mbedtls/debug.h>
#include <cstring>

#include <string>

namespace neustack {

namespace {

void tls_debug_callback(void *, int level, const char *file, int line, const char *str) {
    if (!Logger::instance().should_log(LogModule::TLS, LogLevel::DEBUG)) {
        return;
    }

    std::string message(str ? str : "");
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }

    LOG_DEBUG(TLS, "mbedtls[%d] %s:%d %s", level, file, line, message.c_str());
}

} // namespace

// ─── 析构 ───

TLSContext::~TLSContext() {
    if (_initialized) {
        mbedtls_ssl_config_free(&_ssl_config);
        mbedtls_pk_free(&_pk);
        mbedtls_x509_crt_free(&_cert);
        mbedtls_ctr_drbg_free(&_ctr_drbg);
        mbedtls_entropy_free(&_entropy);
    }
}

// ─── 公共初始化 ───

bool TLSContext::init_common() {
    mbedtls_entropy_init(&_entropy);
    mbedtls_ctr_drbg_init(&_ctr_drbg);
    mbedtls_x509_crt_init(&_cert);
    mbedtls_pk_init(&_pk);
    mbedtls_ssl_config_init(&_ssl_config);

    // 播种 DRBG
    const char *pers = "neustack_tls";
    int ret = mbedtls_ctr_drbg_seed(&_ctr_drbg, mbedtls_entropy_func,
                                     &_entropy,
                                     reinterpret_cast<const unsigned char *>(pers),
                                     std::strlen(pers));
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "ctr_drbg_seed failed: %s (0x%04x)", errbuf, -ret);
        return false;
    }

    _initialized = true;
    mbedtls_ssl_conf_dbg(&_ssl_config, tls_debug_callback, nullptr);
    mbedtls_debug_set_threshold(4);
    return true;
}

// ─── Server 初始化 ───

bool TLSContext::init_server(const std::string &cert_path, const std::string &key_path) {
    _role = TLSRole::Server;

    // 加载证书链
    int ret = mbedtls_x509_crt_parse_file(&_cert, cert_path.c_str());
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "Failed to parse cert '%s': %s (0x%04x)",
                  cert_path.c_str(), errbuf, -ret);
        return false;
    }

    // 加载私钥
    ret = mbedtls_pk_parse_keyfile(&_pk, key_path.c_str(), nullptr,
                                    mbedtls_ctr_drbg_random, &_ctr_drbg);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "Failed to parse key '%s': %s (0x%04x)",
                  key_path.c_str(), errbuf, -ret);
        return false;
    }

    // 配置 SSL
    ret = mbedtls_ssl_config_defaults(&_ssl_config,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "ssl_config_defaults failed: %s (0x%04x)", errbuf, -ret);
        return false;
    }

    mbedtls_ssl_conf_rng(&_ssl_config, mbedtls_ctr_drbg_random, &_ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&_ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&_ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ca_chain(&_ssl_config, _cert.next, nullptr);

    ret = mbedtls_ssl_conf_own_cert(&_ssl_config, &_cert, &_pk);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "ssl_conf_own_cert failed: %s (0x%04x)", errbuf, -ret);
        return false;
    }

    // Server 不验证客户端证书（单向 TLS）
    mbedtls_ssl_conf_authmode(&_ssl_config, MBEDTLS_SSL_VERIFY_NONE);

    LOG_INFO(TLS, "TLS server context initialized (cert=%s)", cert_path.c_str());
    return true;
}

// ─── Client 初始化 ───

bool TLSContext::init_client(const std::string &ca_path) {
    _role = TLSRole::Client;

    // 加载 CA 证书（可选）
    if (!ca_path.empty()) {
        int ret = mbedtls_x509_crt_parse_file(&_cert, ca_path.c_str());
        if (ret != 0) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR(TLS, "Failed to parse CA cert '%s': %s (0x%04x)",
                      ca_path.c_str(), errbuf, -ret);
            return false;
        }
    }

    // 配置 SSL
    int ret = mbedtls_ssl_config_defaults(&_ssl_config,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(TLS, "ssl_config_defaults failed: %s (0x%04x)", errbuf, -ret);
        return false;
    }

    mbedtls_ssl_conf_rng(&_ssl_config, mbedtls_ctr_drbg_random, &_ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&_ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&_ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);

    if (!ca_path.empty()) {
        mbedtls_ssl_conf_ca_chain(&_ssl_config, &_cert, nullptr);
        mbedtls_ssl_conf_authmode(&_ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        // 无 CA = 允许握手继续，但保留并处理对端证书。
        // VERIFY_NONE 在当前 mbedTLS/证书组合下可能导致后续握手路径拿不到 peer cert。
        mbedtls_ssl_conf_authmode(&_ssl_config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }

    LOG_INFO(TLS, "TLS client context initialized (ca=%s)",
             ca_path.empty() ? "<none>" : ca_path.c_str());
    return true;
}

// ─── 工厂方法 ───

std::unique_ptr<TLSContext> TLSContext::create_server(
    const std::string &cert_path,
    const std::string &key_path) {

    auto ctx = std::unique_ptr<TLSContext>(new TLSContext());
    if (!ctx->init_common()) return nullptr;
    if (!ctx->init_server(cert_path, key_path)) return nullptr;
    return ctx;
}

std::unique_ptr<TLSContext> TLSContext::create_client(
    const std::string &ca_path) {

    auto ctx = std::unique_ptr<TLSContext>(new TLSContext());
    if (!ctx->init_common()) return nullptr;
    if (!ctx->init_client(ca_path)) return nullptr;
    return ctx;
}

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
