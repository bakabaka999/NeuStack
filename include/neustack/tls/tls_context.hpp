#ifndef NEUSTACK_TLS_CONTEXT_HPP
#define NEUSTACK_TLS_CONTEXT_HPP

#ifdef NEUSTACK_TLS_ENABLED

#include <string>
#include <memory>

// mbedTLS headers
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>

namespace neustack {

/**
 * TLS 上下文角色
 */
enum class TLSRole {
    Server,
    Client,
};

/**
 * TLSContext — 持有 mbedTLS 配置（证书、密钥、RNG）
 *
 * 每个角色（Server / Client）一个实例，所有连接共享。
 * 线程安全：构造后只读（mbedtls_ssl_config 是 const 使用）。
 */
class TLSContext {
public:
    ~TLSContext();

    TLSContext(const TLSContext &) = delete;
    TLSContext &operator=(const TLSContext &) = delete;

    /**
     * @brief 创建 Server 上下文
     * @param cert_path PEM 证书文件路径
     * @param key_path PEM 私钥文件路径
     * @return nullptr 表示失败
     */
    static std::unique_ptr<TLSContext> create_server(
        const std::string &cert_path,
        const std::string &key_path);

    /**
     * @brief 创建 Client 上下文
     * @param ca_path CA 证书路径（空 = 跳过服务端证书验证）
     * @return nullptr 表示失败
     */
    static std::unique_ptr<TLSContext> create_client(
        const std::string &ca_path = "");

    /**
     * @brief 获取 mbedTLS SSL 配置（只读）
     */
    const mbedtls_ssl_config &ssl_config() const { return _ssl_config; }

    /**
     * @brief 获取角色
     */
    TLSRole role() const { return _role; }

private:
    TLSContext() = default;

    bool init_common();
    bool init_server(const std::string &cert_path, const std::string &key_path);
    bool init_client(const std::string &ca_path);

    TLSRole _role = TLSRole::Server;

    // mbedTLS 对象（顺序重要：析构时反向销毁）
    mbedtls_entropy_context _entropy;
    mbedtls_ctr_drbg_context _ctr_drbg;
    mbedtls_x509_crt _cert;       // Server: own cert chain; Client: CA cert
    mbedtls_pk_context _pk;       // Server: private key
    mbedtls_ssl_config _ssl_config;

    bool _initialized = false;
};

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
#endif // NEUSTACK_TLS_CONTEXT_HPP
