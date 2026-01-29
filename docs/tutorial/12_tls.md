# 教程 12: TLS 安全传输层

## 概述

TLS（Transport Layer Security）是互联网安全通信的基石。它位于 TCP 和应用层之间，提供：

- **加密**: 防止窃听
- **认证**: 验证服务器身份
- **完整性**: 防止数据篡改

本教程将实现 TLS 1.3，让我们的 HTTP 服务器支持 HTTPS。

## TLS 1.3 协议基础

### 为什么选择 TLS 1.3？

TLS 1.3 (RFC 8446) 相比 TLS 1.2 更简洁、更安全：

| 特性 | TLS 1.2 | TLS 1.3 |
|------|---------|---------|
| 握手延迟 | 2-RTT | 1-RTT (0-RTT 可选) |
| 密码套件 | 复杂（数十种） | 精简（5 种） |
| 密钥交换 | RSA 或 ECDHE | 仅 ECDHE |
| 前向安全 | 可选 | 强制 |

### 握手流程（1-RTT）

```
Client                                    Server

ClientHello          -------->
  + key_share
  + supported_versions

                     <--------        ServerHello
                                       + key_share
                                {EncryptedExtensions}
                                    {Certificate*}
                                {CertificateVerify*}
                                        {Finished}
                     <--------  [Application Data*]

{Finished}           -------->
[Application Data]   <------->  [Application Data]
```

`{}` 表示加密发送，`[]` 表示应用数据（也是加密的）。

### 关键概念

**1. 密钥交换 (ECDHE)**

```
Client                          Server
生成 client_private_key          生成 server_private_key
计算 client_public_key           计算 server_public_key

交换 public_key:
client_public_key ------>
                  <------ server_public_key

双方独立计算:
shared_secret = ECDH(my_private, peer_public)
// 双方得到相同的 shared_secret
```

**2. 密钥派生 (HKDF)**

```
shared_secret
    ↓
HKDF-Extract → early_secret → ... → handshake_secret → ... → master_secret
    ↓                                     ↓                         ↓
client_handshake_key              client_app_key
server_handshake_key              server_app_key
```

**3. 记录层加密 (AEAD)**

```
明文 + 附加数据 → AEAD-Encrypt(key, nonce) → 密文 + tag
密文 + tag      → AEAD-Decrypt(key, nonce) → 明文
```

## TLS 记录层

### 记录格式

```
+--+--+--+--+--+
|  ContentType  |  1 字节 (22=handshake, 23=application_data)
+--+--+--+--+--+
| Legacy Version|  2 字节 (0x0303 = TLS 1.2, 兼容性)
+--+--+--+--+--+
|    Length      |  2 字节
+--+--+--+--+--+
|   Fragment     |  变长 (最大 2^14)
+--+--+--+--+--+
```

### ContentType

```cpp
enum class TLSContentType : uint8_t {
    ChangeCipherSpec = 20,  // 兼容
    Alert            = 21,  // 警告
    Handshake        = 22,  // 握手
    ApplicationData  = 23   // 应用数据
};
```

## 实现设计

### 架构概览

```
┌──────────────────────────────────┐
│          Application             │
│        (HTTP Server)             │
├──────────────────────────────────┤
│            TLS Layer             │
│  ┌────────────┬───────────────┐  │
│  │  Handshake  │   Record     │  │
│  │  Protocol   │   Layer      │  │
│  ├────────────┤  ┌─────────┐  │  │
│  │  Key Sched  │  │  AEAD   │  │  │
│  │  (HKDF)    │  │(AES-GCM)│  │  │
│  └────────────┘  └─────────┘  │  │
│  ┌──────────────────────────┐  │  │
│  │     Crypto Primitives     │  │  │
│  │  X25519 / SHA-256 / AES   │  │  │
│  └──────────────────────────┘  │  │
├──────────────────────────────────┤
│           TCP Layer              │
└──────────────────────────────────┘
```

### 文件结构

```
include/neustack/tls/
├── tls_layer.hpp          # TLS 层接口
├── tls_record.hpp         # 记录层
├── tls_handshake.hpp      # 握手协议
├── tls_key_schedule.hpp   # 密钥派生
├── tls_types.hpp          # 类型定义
└── tls_certificate.hpp    # 证书处理

include/neustack/crypto/
├── aes_gcm.hpp            # AES-128-GCM / AES-256-GCM
├── x25519.hpp             # X25519 密钥交换
├── sha256.hpp             # SHA-256 哈希
├── hkdf.hpp               # HKDF 密钥派生
└── chacha20_poly1305.hpp  # ChaCha20-Poly1305 (可选)

src/tls/
├── tls_layer.cpp
├── tls_record.cpp
├── tls_handshake.cpp
└── tls_key_schedule.cpp

src/crypto/
├── aes_gcm.cpp
├── x25519.cpp
├── sha256.cpp
└── hkdf.cpp
```

### 密码学原语

#### X25519 密钥交换

```cpp
// include/neustack/crypto/x25519.hpp
#ifndef NEUSTACK_CRYPTO_X25519_HPP
#define NEUSTACK_CRYPTO_X25519_HPP

#include <array>
#include <cstdint>

namespace neustack::crypto {

/**
 * X25519 椭圆曲线 Diffie-Hellman
 *
 * 基于 Curve25519，提供 128 位安全级别
 * RFC 7748: https://tools.ietf.org/html/rfc7748
 */
class X25519 {
public:
    static constexpr size_t KEY_SIZE = 32;

    using PrivateKey = std::array<uint8_t, KEY_SIZE>;
    using PublicKey = std::array<uint8_t, KEY_SIZE>;
    using SharedSecret = std::array<uint8_t, KEY_SIZE>;

    // 生成随机私钥
    static PrivateKey generate_private_key();

    // 从私钥计算公钥
    static PublicKey compute_public_key(const PrivateKey& private_key);

    // 密钥交换: shared_secret = scalar_mult(private_key, peer_public_key)
    static SharedSecret compute_shared_secret(const PrivateKey& private_key,
                                               const PublicKey& peer_public_key);

private:
    // Curve25519 标量乘法
    // 使用 Montgomery ladder 算法 (恒定时间，防止侧信道攻击)
    static void scalar_mult(uint8_t out[32],
                            const uint8_t scalar[32],
                            const uint8_t point[32]);

    // 域算术 (mod 2^255 - 19)
    using FieldElement = std::array<int64_t, 16>;

    static void fe_add(FieldElement& out, const FieldElement& a, const FieldElement& b);
    static void fe_sub(FieldElement& out, const FieldElement& a, const FieldElement& b);
    static void fe_mul(FieldElement& out, const FieldElement& a, const FieldElement& b);
    static void fe_sq(FieldElement& out, const FieldElement& a);
    static void fe_inv(FieldElement& out, const FieldElement& a);
    static void fe_pack(uint8_t out[32], const FieldElement& a);
    static void fe_unpack(FieldElement& out, const uint8_t in[32]);
};

} // namespace neustack::crypto

#endif
```

#### SHA-256 哈希

```cpp
// include/neustack/crypto/sha256.hpp
#ifndef NEUSTACK_CRYPTO_SHA256_HPP
#define NEUSTACK_CRYPTO_SHA256_HPP

#include <array>
#include <cstdint>
#include <vector>
#include <string>

namespace neustack::crypto {

/**
 * SHA-256 哈希算法
 *
 * FIPS 180-4: https://csrc.nist.gov/publications/detail/fips/180/4/final
 */
class SHA256 {
public:
    static constexpr size_t DIGEST_SIZE = 32;
    static constexpr size_t BLOCK_SIZE = 64;

    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    SHA256();

    // 增量式更新
    void update(const uint8_t* data, size_t len);
    void update(const std::string& data) {
        update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
    void update(const std::vector<uint8_t>& data) {
        update(data.data(), data.size());
    }

    // 完成并返回摘要
    Digest finalize();

    // 一次性计算
    static Digest hash(const uint8_t* data, size_t len);
    static Digest hash(const std::string& data);

private:
    uint32_t _state[8];     // 哈希状态
    uint8_t _buffer[64];    // 块缓冲
    size_t _buffer_len = 0;
    uint64_t _total_len = 0;

    void process_block(const uint8_t block[64]);
};

} // namespace neustack::crypto

#endif
```

#### HMAC 和 HKDF

```cpp
// include/neustack/crypto/hkdf.hpp
#ifndef NEUSTACK_CRYPTO_HKDF_HPP
#define NEUSTACK_CRYPTO_HKDF_HPP

#include "sha256.hpp"
#include <vector>

namespace neustack::crypto {

/**
 * HMAC-SHA256
 */
class HMAC_SHA256 {
public:
    static SHA256::Digest compute(const uint8_t* key, size_t key_len,
                                   const uint8_t* data, size_t data_len);
};

/**
 * HKDF (HMAC-based Key Derivation Function)
 * RFC 5869: https://tools.ietf.org/html/rfc5869
 *
 * TLS 1.3 使用 HKDF 进行所有密钥派生
 */
class HKDF {
public:
    // HKDF-Extract: PRK = HMAC(salt, IKM)
    static SHA256::Digest extract(const uint8_t* salt, size_t salt_len,
                                   const uint8_t* ikm, size_t ikm_len);

    // HKDF-Expand: OKM = HMAC(PRK, info || 0x01) || ...
    static std::vector<uint8_t> expand(const uint8_t* prk, size_t prk_len,
                                        const uint8_t* info, size_t info_len,
                                        size_t output_len);

    // TLS 1.3 专用: HKDF-Expand-Label
    // 格式: HKDF-Expand(Secret, HkdfLabel, Length)
    // HkdfLabel = length || "tls13 " || label || context
    static std::vector<uint8_t> expand_label(const uint8_t* secret, size_t secret_len,
                                              const std::string& label,
                                              const uint8_t* context, size_t context_len,
                                              size_t output_len);

    // TLS 1.3: Derive-Secret
    // Derive-Secret(Secret, Label, Messages) =
    //   HKDF-Expand-Label(Secret, Label, Hash(Messages), Hash.length)
    static SHA256::Digest derive_secret(const uint8_t* secret, size_t secret_len,
                                         const std::string& label,
                                         const uint8_t* messages_hash,
                                         size_t hash_len);
};

} // namespace neustack::crypto

#endif
```

#### AES-128-GCM

```cpp
// include/neustack/crypto/aes_gcm.hpp
#ifndef NEUSTACK_CRYPTO_AES_GCM_HPP
#define NEUSTACK_CRYPTO_AES_GCM_HPP

#include <array>
#include <vector>
#include <cstdint>
#include <optional>

namespace neustack::crypto {

/**
 * AES-128-GCM (Galois/Counter Mode)
 *
 * TLS 1.3 默认的 AEAD 算法
 * 提供加密 + 认证
 */
class AES128GCM {
public:
    static constexpr size_t KEY_SIZE = 16;
    static constexpr size_t NONCE_SIZE = 12;
    static constexpr size_t TAG_SIZE = 16;

    using Key = std::array<uint8_t, KEY_SIZE>;
    using Nonce = std::array<uint8_t, NONCE_SIZE>;
    using Tag = std::array<uint8_t, TAG_SIZE>;

    explicit AES128GCM(const Key& key);

    // AEAD 加密
    // 返回: 密文 + tag
    std::vector<uint8_t> encrypt(const Nonce& nonce,
                                  const uint8_t* plaintext, size_t plaintext_len,
                                  const uint8_t* aad, size_t aad_len);

    // AEAD 解密
    // 返回: 明文 (验证失败返回 nullopt)
    std::optional<std::vector<uint8_t>> decrypt(
        const Nonce& nonce,
        const uint8_t* ciphertext, size_t ciphertext_len,  // 包含 tag
        const uint8_t* aad, size_t aad_len);

private:
    // AES 密钥扩展
    std::array<uint32_t, 44> _round_keys;

    // GCM 乘法表 (H)
    std::array<uint8_t, 16> _H;

    // AES 加密单块
    void aes_encrypt_block(uint8_t out[16], const uint8_t in[16]) const;

    // GF(2^128) 乘法
    static void ghash_multiply(uint8_t out[16],
                                const uint8_t H[16],
                                const uint8_t X[16]);

    // GHASH
    void ghash(uint8_t out[16],
               const uint8_t* aad, size_t aad_len,
               const uint8_t* ciphertext, size_t ct_len) const;

    // GCTR (计数器模式加密)
    void gctr(uint8_t* out, const uint8_t* in, size_t len,
              const uint8_t icb[16]) const;
};

} // namespace neustack::crypto

#endif
```

### TLS 密钥调度

```cpp
// include/neustack/tls/tls_key_schedule.hpp
#ifndef NEUSTACK_TLS_KEY_SCHEDULE_HPP
#define NEUSTACK_TLS_KEY_SCHEDULE_HPP

#include "neustack/crypto/hkdf.hpp"
#include "neustack/crypto/sha256.hpp"

namespace neustack::tls {

/**
 * TLS 1.3 密钥调度
 *
 * 密钥派生过程:
 *
 *              0
 *              |
 *              v
 *    PSK ->  HKDF-Extract = Early Secret
 *              |
 *              +-> Derive-Secret(., "c e traffic", ClientHello)
 *              |   = client_early_traffic_secret
 *              |
 *              v
 *        Derive-Secret(., "derived", "")
 *              |
 *              v
 *    ECDHE -> HKDF-Extract = Handshake Secret
 *              |
 *              +-> Derive-Secret(., "c hs traffic", ClientHello...ServerHello)
 *              |   = client_handshake_traffic_secret
 *              |
 *              +-> Derive-Secret(., "s hs traffic", ClientHello...ServerHello)
 *              |   = server_handshake_traffic_secret
 *              |
 *              v
 *        Derive-Secret(., "derived", "")
 *              |
 *              v
 *    0 ->    HKDF-Extract = Master Secret
 *              |
 *              +-> Derive-Secret(., "c ap traffic", ClientHello...Finished)
 *              |   = client_application_traffic_secret
 *              |
 *              +-> Derive-Secret(., "s ap traffic", ClientHello...Finished)
 *              |   = server_application_traffic_secret
 */
class TLSKeySchedule {
public:
    TLSKeySchedule();

    // 步骤 1: 计算 Early Secret (无 PSK 时 ikm = 全零)
    void compute_early_secret(const uint8_t* psk = nullptr, size_t psk_len = 0);

    // 步骤 2: 计算 Handshake Secret
    void compute_handshake_secret(const uint8_t* shared_secret, size_t len);

    // 步骤 3: 派生握手流量密钥
    void derive_handshake_keys(const uint8_t* transcript_hash);

    // 步骤 4: 计算 Master Secret
    void compute_master_secret();

    // 步骤 5: 派生应用流量密钥
    void derive_application_keys(const uint8_t* transcript_hash);

    // 获取密钥
    struct TrafficKeys {
        std::array<uint8_t, 16> key;   // AES-128 key
        std::array<uint8_t, 12> iv;    // Nonce
    };

    const TrafficKeys& client_handshake_keys() const { return _client_hs_keys; }
    const TrafficKeys& server_handshake_keys() const { return _server_hs_keys; }
    const TrafficKeys& client_app_keys() const { return _client_app_keys; }
    const TrafficKeys& server_app_keys() const { return _server_app_keys; }

    // Finished 消息的密钥
    const crypto::SHA256::Digest& client_finished_key() const { return _client_finished_key; }
    const crypto::SHA256::Digest& server_finished_key() const { return _server_finished_key; }

private:
    crypto::SHA256::Digest _early_secret;
    crypto::SHA256::Digest _handshake_secret;
    crypto::SHA256::Digest _master_secret;

    TrafficKeys _client_hs_keys;
    TrafficKeys _server_hs_keys;
    TrafficKeys _client_app_keys;
    TrafficKeys _server_app_keys;

    crypto::SHA256::Digest _client_finished_key;
    crypto::SHA256::Digest _server_finished_key;
};

} // namespace neustack::tls

#endif
```

### TLS 层接口

```cpp
// include/neustack/tls/tls_layer.hpp
#ifndef NEUSTACK_TLS_LAYER_HPP
#define NEUSTACK_TLS_LAYER_HPP

#include "tls_types.hpp"
#include "tls_key_schedule.hpp"
#include "tls_record.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/crypto/x25519.hpp"
#include "neustack/crypto/aes_gcm.hpp"

namespace neustack::tls {

/**
 * TLS 层
 *
 * 服务端使用:
 *   TLSServer tls(tcp_layer);
 *   tls.set_certificate(cert_pem, key_pem);
 *
 *   tls.listen(443, [](TLSConnection* conn) {
 *       return TLSCallbacks{
 *           .on_receive = [](TLSConnection* conn, const uint8_t* data, size_t len) {
 *               // 收到解密后的应用数据
 *           },
 *           .on_close = [](TLSConnection* conn) {
 *               // 连接关闭
 *           }
 *       };
 *   });
 */

// TLS 连接状态
struct TLSConnection {
    TCB* tcb;                    // 底层 TCP 连接
    TLSState state;              // 握手状态

    // 密钥调度
    TLSKeySchedule key_schedule;

    // ECDHE 密钥对
    crypto::X25519::PrivateKey our_private_key;
    crypto::X25519::PublicKey our_public_key;

    // 加密上下文
    std::unique_ptr<crypto::AES128GCM> send_cipher;
    std::unique_ptr<crypto::AES128GCM> recv_cipher;
    std::array<uint8_t, 12> send_iv;
    std::array<uint8_t, 12> recv_iv;
    uint64_t send_seq = 0;       // 发送序列号 (用于 nonce)
    uint64_t recv_seq = 0;       // 接收序列号

    // 握手摘要 (累积所有握手消息的哈希)
    crypto::SHA256 transcript;

    // 接收缓冲区 (处理 TLS 记录跨 TCP 段的情况)
    std::vector<uint8_t> recv_buffer;
};

// TLS 回调
struct TLSCallbacks {
    std::function<void(TLSConnection*, const uint8_t*, size_t)> on_receive;
    std::function<void(TLSConnection*)> on_close;
};

using TLSAcceptCallback = std::function<TLSCallbacks(TLSConnection*)>;

class TLSServer {
public:
    explicit TLSServer(TCPLayer& tcp);

    // 加载证书和私钥 (PEM 格式)
    bool set_certificate(const std::string& cert_pem, const std::string& key_pem);

    // 开始监听
    int listen(uint16_t port, TLSAcceptCallback on_accept);

    // 发送应用数据（加密后发送）
    ssize_t send(TLSConnection* conn, const uint8_t* data, size_t len);

    // 关闭连接
    void close(TLSConnection* conn);

private:
    TCPLayer& _tcp;
    std::vector<uint8_t> _certificate_der;
    crypto::X25519::PrivateKey _server_key;  // 简化：使用 X25519 作为身份密钥

    TLSAcceptCallback _on_accept;
    std::unordered_map<TCB*, std::unique_ptr<TLSConnection>> _connections;
    std::unordered_map<TCB*, TLSCallbacks> _callbacks;

    // TCP 回调
    void on_tcp_receive(TCB* tcb, const uint8_t* data, size_t len);
    void on_tcp_close(TCB* tcb);

    // 记录层
    void process_record(TLSConnection* conn, TLSContentType type,
                        const uint8_t* data, size_t len);
    void send_record(TLSConnection* conn, TLSContentType type,
                     const uint8_t* data, size_t len);

    // 握手
    void handle_client_hello(TLSConnection* conn, const uint8_t* data, size_t len);
    void send_server_hello(TLSConnection* conn);
    void send_encrypted_extensions(TLSConnection* conn);
    void send_certificate(TLSConnection* conn);
    void send_certificate_verify(TLSConnection* conn);
    void send_finished(TLSConnection* conn);
    void handle_finished(TLSConnection* conn, const uint8_t* data, size_t len);

    // 加密/解密
    std::vector<uint8_t> encrypt_record(TLSConnection* conn,
                                         TLSContentType type,
                                         const uint8_t* data, size_t len);
    std::optional<std::pair<TLSContentType, std::vector<uint8_t>>>
    decrypt_record(TLSConnection* conn, const uint8_t* data, size_t len);

    // 计算 nonce: IV XOR seq_num
    static std::array<uint8_t, 12> compute_nonce(
        const std::array<uint8_t, 12>& iv, uint64_t seq);
};

} // namespace neustack::tls

#endif
```

## 使用示例

### HTTPS 服务器

```cpp
#include "neustack/tls/tls_layer.hpp"
#include "neustack/app/http_server.hpp"

int main() {
    // ... 初始化 HAL, IP, TCP ...

    // 创建 TLS 层
    tls::TLSServer tls(tcp_layer);

    // 加载证书（可以用 openssl 生成自签名证书）
    tls.set_certificate("server.crt", "server.key");

    // 创建 HTTPS 服务器
    tls.listen(443, [](tls::TLSConnection* conn) {
        // 每个 TLS 连接创建一个 HTTP 解析器
        auto parser = std::make_shared<HttpParser>();

        return tls::TLSCallbacks{
            .on_receive = [parser, &tls](tls::TLSConnection* conn,
                                          const uint8_t* data, size_t len) {
                parser->feed(data, len);

                if (parser->is_complete()) {
                    auto request = parser->take_request();

                    // 构建响应
                    HttpResponse response;
                    response.content_type("text/html");
                    response.set_body("<h1>Secure Hello!</h1>"
                                     "<p>This is served over HTTPS.</p>");

                    auto data = response.serialize();
                    tls.send(conn,
                            reinterpret_cast<const uint8_t*>(data.data()),
                            data.size());

                    parser->reset();
                }
            },
            .on_close = [](tls::TLSConnection* conn) {
                // 连接关闭
            }
        };
    });

    // 主循环...
}
```

### 生成自签名证书

```bash
# 生成 EC 私钥 (X25519 用于密钥交换，Ed25519 用于签名)
openssl genpkey -algorithm ED25519 -out server.key

# 生成自签名证书
openssl req -new -x509 -key server.key -out server.crt -days 365 \
    -subj "/CN=192.168.100.2"

# 或者使用 RSA (兼容性更好)
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 365 -nodes -subj "/CN=192.168.100.2"
```

### 测试

```bash
# 使用 curl 测试 (忽略自签名证书)
curl -k https://192.168.100.2/

# 使用 openssl 调试
openssl s_client -connect 192.168.100.2:443 -tls1_3

# 查看证书信息
openssl x509 -in server.crt -text -noout
```

## 实现顺序建议

TLS 的实现相当复杂，建议分步进行：

### 第一步：密码学原语

```
1. SHA-256        → 哈希算法（TLS 到处都用）
2. HMAC-SHA256    → 消息认证码
3. HKDF           → 密钥派生
4. X25519         → 密钥交换
5. AES-128-GCM    → 对称加密
```

### 第二步：记录层

```
6. TLS 记录解析/构建
7. 记录加密/解密
```

### 第三步：握手协议

```
8.  ClientHello 解析
9.  ServerHello 构建与发送
10. 密钥交换与密钥派生
11. EncryptedExtensions
12. Certificate
13. CertificateVerify
14. Finished (双方)
```

### 第四步：应用数据

```
15. 加密应用数据收发
16. 与 HTTP 服务器集成
```

## 安全注意事项

| 安全点 | 措施 |
|--------|------|
| 恒定时间比较 | 防止时序攻击 |
| 密钥擦除 | 使用后清零内存 |
| 随机数质量 | 使用 `/dev/urandom` |
| 证书验证 | 检查签名链 |
| 降级防护 | 仅支持 TLS 1.3 |

## 扩展思考

### 简化方案

完整的 TLS 实现非常复杂。可以考虑：

1. **集成 BearSSL**: 嵌入式友好的 TLS 库，API 简洁
2. **集成 wolfSSL**: 轻量级，适合资源受限环境
3. **只实现握手**: 理解核心原理，加密用现成库

### 进阶话题

1. **0-RTT 恢复**: 减少连接延迟
2. **客户端认证**: 双向 TLS
3. **OCSP Stapling**: 证书状态检查
4. **证书透明度**: 防止伪造证书

## 小结

本教程设计了 TLS 1.3 的实现方案，核心要点：

1. **1-RTT 握手**: ECDHE 密钥交换 + 证书认证
2. **HKDF 密钥派生**: 从共享密钥派生所有流量密钥
3. **AEAD 加密**: AES-GCM 提供加密 + 完整性
4. **记录层**: 所有数据分记录传输

TLS 是整个协议栈中最复杂的部分。建议先完成密码学原语，再逐步构建上层协议。

## 第一阶段总结

至此，NeuStack 的第一阶段开发完成，我们实现了：

```
应用层:   HTTP Server + DNS Client + TLS
传输层:   TCP (可靠传输、流控、拥塞控制) + UDP
网络层:   IPv4 + ICMP + ARP
链路层:   macOS utun HAL
```

这是一个完整的、可工作的用户态 TCP/IP 协议栈。


