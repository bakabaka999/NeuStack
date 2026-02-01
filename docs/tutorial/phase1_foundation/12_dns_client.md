# 教程 12: DNS 客户端实现

## 概述

DNS（Domain Name System）是互联网的"电话簿"，将人类可读的域名转换为 IP 地址。本教程将实现一个 DNS 客户端，让我们的协议栈能够：

- 解析域名为 IP 地址
- 理解 DNS 协议格式
- 使用 UDP 进行查询

结合上一教程的 HTTP 客户端，我们就能实现通过域名访问网站的完整功能。

## DNS 协议基础

### 查询流程

```
应用程序: 我要访问 www.example.com
    ↓
DNS 客户端: 向 DNS 服务器发送查询
    ↓
DNS 服务器 (8.8.8.8): 返回 93.184.216.34
    ↓
应用程序: 连接到 93.184.216.34
```

### 报文格式

DNS 使用 UDP 端口 53，报文格式如下：

```
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                      ID                       |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|QR|   Opcode  |AA|TC|RD|RA|   Z    |   RCODE   |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    QDCOUNT                    |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    ANCOUNT                    |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    NSCOUNT                    |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    ARCOUNT                    |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                   Questions                   |  变长
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    Answers                    |  变长
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

#### 头部字段

| 字段 | 大小 | 说明 |
|------|------|------|
| ID | 16 位 | 事务 ID，匹配请求和响应 |
| QR | 1 位 | 0=查询，1=响应 |
| Opcode | 4 位 | 0=标准查询 |
| AA | 1 位 | 权威回答 |
| TC | 1 位 | 截断标志 |
| RD | 1 位 | 期望递归 |
| RA | 1 位 | 支持递归 |
| RCODE | 4 位 | 响应码 (0=成功) |
| QDCOUNT | 16 位 | 问题数 |
| ANCOUNT | 16 位 | 回答数 |

#### 问题区格式

```
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                     QNAME                     |  变长 (域名编码)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                     QTYPE                     |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                     QCLASS                    |  2 字节
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

#### 域名编码

`www.example.com` 编码为：
```
\x03www\x07example\x03com\x00
```

每个标签前有长度字节，以 `\x00` 结尾。

## 实现设计

### 文件结构

```
include/neustack/app/
└── dns_client.hpp

src/app/
└── dns_client.cpp
```

### DNS 类型定义

```cpp
// include/neustack/app/dns_client.hpp
#ifndef NEUSTACK_APP_DNS_CLIENT_HPP
#define NEUSTACK_APP_DNS_CLIENT_HPP

#include "neustack/transport/udp.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>
#include <chrono>

namespace neustack {

// DNS 记录类型
enum class DNSType : uint16_t {
    A     = 1,      // IPv4 地址
    NS    = 2,      // 域名服务器
    CNAME = 5,      // 规范名称
    SOA   = 6,      // 起始授权
    PTR   = 12,     // 指针记录
    MX    = 15,     // 邮件交换
    TXT   = 16,     // 文本记录
    AAAA  = 28,     // IPv6 地址
    SRV   = 33      // 服务定位
};

// DNS 类
enum class DNSClass : uint16_t {
    IN = 1          // Internet
};

// DNS 响应码
enum class DNSRcode : uint8_t {
    NoError  = 0,
    FormErr  = 1,   // 格式错误
    ServFail = 2,   // 服务器失败
    NXDomain = 3,   // 域名不存在
    NotImp   = 4,   // 未实现
    Refused  = 5    // 拒绝
};

// DNS 记录
struct DNSRecord {
    std::string name;
    DNSType type;
    DNSClass cls;
    uint32_t ttl;

    // 记录数据
    std::variant<
        uint32_t,           // A 记录: IPv4 地址
        std::string,        // CNAME/NS/PTR: 域名
        std::vector<uint8_t> // 其他: 原始数据
    > data;

    // 便捷方法
    std::optional<uint32_t> as_ipv4() const;
    std::optional<std::string> as_name() const;
};

// DNS 响应
struct DNSResponse {
    uint16_t id;
    DNSRcode rcode;
    std::vector<DNSRecord> answers;
    std::vector<DNSRecord> authorities;
    std::vector<DNSRecord> additionals;

    // 获取第一个 A 记录
    std::optional<uint32_t> get_ip() const {
        for (const auto& ans : answers) {
            if (ans.type == DNSType::A) {
                return ans.as_ipv4();
            }
        }
        return std::nullopt;
    }
};

// 解析结果回调
using DNSCallback = std::function<void(std::optional<DNSResponse>)>;

/**
 * DNS 客户端
 *
 * 异步、非阻塞设计（符合单线程协议栈架构）
 *
 * 使用示例:
 *   DNSClient dns(udp_handler, 0x08080808);  // Google DNS
 *   dns.init();
 *
 *   dns.resolve_async("www.example.com", [](auto response) {
 *       if (response) {
 *           auto ip = response->get_ip();
 *       }
 *   });
 */
class DNSClient {
public:
    DNSClient(UDPHandler& udp, uint32_t dns_server = 0x08080808)
        : _udp(udp), _dns_server(dns_server) {}

    // 设置 DNS 服务器
    void set_server(uint32_t ip) { _dns_server = ip; }

    // 异步解析
    void resolve_async(const std::string& hostname, DNSCallback callback,
                       DNSType type = DNSType::A);

    // 处理收到的 DNS 响应（由 UDP 层调用）
    void on_receive(uint32_t src_ip, uint16_t src_port,
                    const uint8_t* data, size_t len);

    // 定时器处理（超时重传）
    void on_timer();

    // 获取绑定的本地端口
    uint16_t local_port() const { return _local_port; }

    // 初始化（绑定 UDP 端口）
    bool init();

private:
    UDPHandler& _udp;
    uint32_t _dns_server;
    uint16_t _local_port = 0;
    uint16_t _next_id = 1;

    // 等待中的查询
    struct PendingQuery {
        uint16_t id;
        std::string hostname;
        DNSType type;
        DNSCallback callback;
        std::chrono::steady_clock::time_point send_time;
        int retries = 0;
    };
    std::unordered_map<uint16_t, PendingQuery> _pending;

    // 构建查询报文
    std::vector<uint8_t> build_query(uint16_t id, const std::string& hostname,
                                      DNSType type);

    // 解析响应报文
    std::optional<DNSResponse> parse_response(const uint8_t* data, size_t len);

    // 编码域名
    static std::vector<uint8_t> encode_name(const std::string& name);

    // 解码域名
    static std::string decode_name(const uint8_t* data, size_t len,
                                   size_t& offset);
};

} // namespace neustack

#endif
```

### DNS 客户端实现

```cpp
// src/app/dns_client.cpp
#include "neustack/app/dns_client.hpp"
#include "neustack/common/log.hpp"
#include <cstring>
#include <random>

using namespace neustack;

// DNS 头部结构
struct DNSHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};
static_assert(sizeof(DNSHeader) == 12, "DNSHeader must be 12 bytes");

bool DNSClient::init() {
    // 随机选择一个本地端口
    std::random_device rd;
    _local_port = 10000 + (rd() % 50000);

    // 注册 UDP 回调
    _udp.bind(_local_port, [this](uint32_t src_ip, uint16_t src_port,
                                   const uint8_t* data, size_t len) {
        on_receive(src_ip, src_port, data, len);
    });

    LOG_INFO(DNS, "DNS client initialized, local port %u", _local_port);
    return true;
}

void DNSClient::resolve_async(const std::string& hostname, DNSCallback callback,
                               DNSType type) {
    uint16_t id = _next_id++;

    // 记录待处理查询
    PendingQuery query;
    query.id = id;
    query.hostname = hostname;
    query.type = type;
    query.callback = std::move(callback);
    query.send_time = std::chrono::steady_clock::now();

    _pending[id] = std::move(query);

    // 构建并发送查询
    auto packet = build_query(id, hostname, type);

    _udp.send(_dns_server, 53, _local_port, packet.data(), packet.size());

    LOG_DEBUG(DNS, "Query sent: %s (id=%u)", hostname.c_str(), id);
}

void DNSClient::on_receive(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, size_t len) {
    // 验证来源
    if (src_ip != _dns_server || src_port != 53) {
        return;
    }

    auto response = parse_response(data, len);
    if (!response) {
        LOG_WARN(DNS, "Failed to parse DNS response");
        return;
    }

    // 查找对应的待处理查询
    auto it = _pending.find(response->id);
    if (it == _pending.end()) {
        LOG_WARN(DNS, "Unknown transaction ID: %u", response->id);
        return;
    }

    LOG_DEBUG(DNS, "Response received: %s -> %u answers",
              it->second.hostname.c_str(),
              static_cast<unsigned>(response->answers.size()));

    // 调用回调
    it->second.callback(std::move(response));

    // 移除待处理查询
    _pending.erase(it);
}

void DNSClient::on_timer() {
    auto now = std::chrono::steady_clock::now();

    for (auto it = _pending.begin(); it != _pending.end(); ) {
        auto elapsed = now - it->second.send_time;

        if (elapsed > std::chrono::seconds(2)) {
            if (it->second.retries >= 3) {
                // 超时，通知失败
                LOG_WARN(DNS, "Query timeout: %s", it->second.hostname.c_str());
                it->second.callback(std::nullopt);
                it = _pending.erase(it);
            } else {
                // 重传
                it->second.retries++;
                it->second.send_time = now;

                auto packet = build_query(it->second.id, it->second.hostname,
                                          it->second.type);
                _udp.send(_dns_server, 53, _local_port,
                          packet.data(), packet.size());

                LOG_DEBUG(DNS, "Query retry #%d: %s",
                          it->second.retries, it->second.hostname.c_str());
                ++it;
            }
        } else {
            ++it;
        }
    }
}

std::vector<uint8_t> DNSClient::build_query(uint16_t id,
                                             const std::string& hostname,
                                             DNSType type) {
    std::vector<uint8_t> packet;

    // 预留空间
    packet.resize(sizeof(DNSHeader));

    // 填充头部
    auto* hdr = reinterpret_cast<DNSHeader*>(packet.data());
    hdr->id = htons(id);
    hdr->flags = htons(0x0100);  // RD=1 (请求递归)
    hdr->qdcount = htons(1);     // 1 个问题
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    // 添加问题区
    auto name = encode_name(hostname);
    packet.insert(packet.end(), name.begin(), name.end());

    // QTYPE
    uint16_t qtype = htons(static_cast<uint16_t>(type));
    packet.push_back((qtype >> 8) & 0xFF);
    packet.push_back(qtype & 0xFF);

    // QCLASS (IN)
    uint16_t qclass = htons(1);
    packet.push_back((qclass >> 8) & 0xFF);
    packet.push_back(qclass & 0xFF);

    return packet;
}

std::optional<DNSResponse> DNSClient::parse_response(const uint8_t* data,
                                                       size_t len) {
    if (len < sizeof(DNSHeader)) {
        return std::nullopt;
    }

    auto* hdr = reinterpret_cast<const DNSHeader*>(data);

    DNSResponse response;
    response.id = ntohs(hdr->id);

    uint16_t flags = ntohs(hdr->flags);
    response.rcode = static_cast<DNSRcode>(flags & 0x0F);

    // 检查是否是响应
    if (!(flags & 0x8000)) {
        return std::nullopt;  // 不是响应
    }

    uint16_t qdcount = ntohs(hdr->qdcount);
    uint16_t ancount = ntohs(hdr->ancount);

    size_t offset = sizeof(DNSHeader);

    // 跳过问题区
    for (uint16_t i = 0; i < qdcount && offset < len; i++) {
        decode_name(data, len, offset);  // 跳过 QNAME
        offset += 4;  // 跳过 QTYPE 和 QCLASS
    }

    // 解析回答区
    for (uint16_t i = 0; i < ancount && offset < len; i++) {
        DNSRecord record;

        record.name = decode_name(data, len, offset);

        if (offset + 10 > len) break;

        record.type = static_cast<DNSType>(
            (data[offset] << 8) | data[offset + 1]);
        offset += 2;

        record.cls = static_cast<DNSClass>(
            (data[offset] << 8) | data[offset + 1]);
        offset += 2;

        record.ttl = (data[offset] << 24) | (data[offset + 1] << 16) |
                     (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;

        uint16_t rdlength = (data[offset] << 8) | data[offset + 1];
        offset += 2;

        if (offset + rdlength > len) break;

        // 解析记录数据
        if (record.type == DNSType::A && rdlength == 4) {
            uint32_t ip = (data[offset] << 24) | (data[offset + 1] << 16) |
                          (data[offset + 2] << 8) | data[offset + 3];
            record.data = ip;
        } else if (record.type == DNSType::CNAME ||
                   record.type == DNSType::NS) {
            size_t name_offset = offset;
            record.data = decode_name(data, len, name_offset);
        } else {
            std::vector<uint8_t> raw(data + offset, data + offset + rdlength);
            record.data = std::move(raw);
        }

        offset += rdlength;
        response.answers.push_back(std::move(record));
    }

    return response;
}

std::vector<uint8_t> DNSClient::encode_name(const std::string& name) {
    std::vector<uint8_t> result;

    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('.', start);
        if (end == std::string::npos) {
            end = name.size();
        }

        size_t len = end - start;
        if (len > 63) len = 63;  // 标签最大 63 字节

        result.push_back(static_cast<uint8_t>(len));
        result.insert(result.end(), name.begin() + start, name.begin() + end);

        start = end + 1;
    }

    result.push_back(0);  // 结束标记
    return result;
}

std::string DNSClient::decode_name(const uint8_t* data, size_t len,
                                    size_t& offset) {
    std::string result;
    bool first = true;
    int jumps = 0;
    size_t original_offset = offset;
    bool jumped = false;

    while (offset < len) {
        uint8_t label_len = data[offset];

        // 检查是否是指针（压缩）
        if ((label_len & 0xC0) == 0xC0) {
            if (offset + 1 >= len) break;

            uint16_t ptr = ((label_len & 0x3F) << 8) | data[offset + 1];

            if (!jumped) {
                original_offset = offset + 2;
                jumped = true;
            }

            offset = ptr;
            jumps++;
            if (jumps > 10) break;  // 防止无限循环
            continue;
        }

        if (label_len == 0) {
            offset++;
            break;
        }

        offset++;
        if (offset + label_len > len) break;

        if (!first) result += '.';
        first = false;

        result.append(reinterpret_cast<const char*>(data + offset), label_len);
        offset += label_len;
    }

    if (jumped) {
        offset = original_offset;
    }

    return result;
}

std::optional<uint32_t> DNSRecord::as_ipv4() const {
    if (auto* ip = std::get_if<uint32_t>(&data)) {
        return *ip;
    }
    return std::nullopt;
}

std::optional<std::string> DNSRecord::as_name() const {
    if (auto* name = std::get_if<std::string>(&data)) {
        return *name;
    }
    return std::nullopt;
}
```

## 使用示例

### 基本用法

```cpp
#include "neustack/app/dns_client.hpp"

int main() {
    // ... 初始化 HAL, IP, UDP ...

    // 创建 DNS 客户端
    DNSClient dns(udp_handler);
    dns.set_server(0x08080808);  // 8.8.8.8 (Google DNS)
    dns.init();

    // 异步解析
    dns.resolve_async("www.example.com", [](auto response) {
        if (response) {
            if (auto ip = response->get_ip()) {
                printf("Resolved: %s\n", ip_to_string(*ip).c_str());
            }
        } else {
            printf("DNS query failed\n");
        }
    });

    // 主循环需要处理 UDP 接收和定时器
    while (running) {
        // ... 接收数据包 ...
        dns.on_timer();
    }
}
```

### 集成 HTTP 客户端

结合教程 11 的 `HttpClient`：

```cpp
void fetch_url(const std::string& hostname, DNSClient& dns, HttpClient& http) {
    // DNS 解析
    dns.resolve_async(hostname, [&http, hostname](auto response) {
        if (!response) {
            printf("DNS lookup failed\n");
            return;
        }

        auto ip = response->get_ip();
        if (!ip) {
            printf("No A record found\n");
            return;
        }

        printf("Resolved %s -> %s\n", hostname.c_str(), ip_to_string(*ip).c_str());

        // 使用 HttpClient 发起请求
        http.set_default_host(hostname);
        http.get(*ip, 80, "/", [](const HttpResponse& resp, int error) {
            if (error != 0) {
                printf("HTTP request failed: %d\n", error);
                return;
            }

            printf("HTTP %d, body: %zu bytes\n",
                   static_cast<int>(resp.status), resp.body.size());
        });
    });
}

// 使用
fetch_url("www.example.com", dns, http);
```

## 测试

```bash
# 使用 dig 对比验证
dig www.example.com @8.8.8.8

# 使用 tcpdump 抓包分析
sudo tcpdump -i utun3 udp port 53 -vvv
```

## DNS 缓存

为了提高性能，可以添加简单的缓存：

```cpp
class DNSCache {
public:
    void put(const std::string& hostname, uint32_t ip, uint32_t ttl) {
        auto expire = std::chrono::steady_clock::now() +
                      std::chrono::seconds(ttl);
        _cache[hostname] = {ip, expire};
    }

    std::optional<uint32_t> get(const std::string& hostname) {
        auto it = _cache.find(hostname);
        if (it == _cache.end()) return std::nullopt;

        if (std::chrono::steady_clock::now() > it->second.expire) {
            _cache.erase(it);
            return std::nullopt;
        }

        return it->second.ip;
    }

private:
    struct Entry {
        uint32_t ip;
        std::chrono::steady_clock::time_point expire;
    };
    std::unordered_map<std::string, Entry> _cache;
};
```

## 扩展思考

### 功能扩展

1. **AAAA 记录**: IPv6 支持
2. **MX 记录**: 邮件服务器查询
3. **反向查询**: IP 到域名
4. **DNS over TCP**: 大响应支持

### 安全考虑

1. **DNS 劫持**: 验证响应来源
2. **缓存投毒**: 随机化端口和 ID
3. **DoH/DoT**: DNS over HTTPS/TLS

## 小结

本教程实现了 DNS 客户端，核心要点：

1. **报文格式**: 头部 + 问题 + 回答
2. **域名编码**: 长度前缀标签
3. **名称压缩**: 指针引用
4. **异步模式**: 回调处理响应

下一教程将实现 TLS，为 HTTP 添加安全传输能力。
