#ifndef NEUSTACK_APP_DNS_CLIENT_HPP
#define NEUSTACK_APP_DNS_CLIENT_HPP

#include "neustack/transport/udp.hpp"
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <functional>
#include <unordered_map>
#include <chrono>

namespace neustack {
    
// DNS 记录类型
enum class DNSType : uint16_t{
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
        uint32_t,               // A 记录：IPv4地址
        std::string,            // CNAME/NS/PTR：域名
        std::vector<uint8_t>    // 其他：原始数据
    > data;

    // 便捷方法
    std::optional<uint32_t> as_ipv4() const {
        if (auto *ip = std::get_if<uint32_t>(&data)) {
            return *ip;
        }
        return std::nullopt;
    }
    std::optional<std::string> as_name() const {
        if (auto *name = std::get_if<std::string>(&data)) {
            return *name;
        }
        return std::nullopt;
    }
};

// DNS 响应
struct DNSResponse {
    uint16_t id;                        // 事务 ID
    DNSRcode rcode;                     // 响应状态码
    std::vector<DNSRecord> answers;     // 回答部分
    std::vector<DNSRecord> authorities; // 授权部分
    std::vector<DNSRecord> additionals; // 附加部分

    // 获取第一个 A 记录
    std::optional<uint32_t> get_ip() const {
        for (const auto &ans : answers) {
            if (ans.type == DNSType::A) {
                return ans.as_ipv4();
            }
        }
        return std::nullopt;
    }
};

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

// 解析结果回调
using DNSCallback = std::function<void(std::optional<DNSResponse>)>;

/**
 * DNS 客户端
 *
 * 异步、非阻塞设计（符合单线程协议栈架构）
 *
 * 使用示例:
 *   DNSClient dns(udp_layer, 0x08080808);  // Google DNS
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
    DNSClient(UDPLayer &udp, uint32_t dns_server = 0x08080808)
        : _udp(udp), _dns_server(dns_server) {}

    // 设置 DNS 服务器
    void set_server(uint32_t ip) { _dns_server = ip; }

    // 异步解析
    void resolve_async(const std::string &hostname, DNSCallback callback,
                       DNSType type = DNSType::A);

    // 处理收到的 DNS 解析
    void on_receive(uint32_t src_ip, uint16_t src_port,
                    const uint8_t *data, size_t len);

    // 定时器处理（超时重传）
    void on_timer();

    // 获取绑定的本地端口
    uint16_t local_port() const { return _local_port; }

    // 初始化（绑定 UDP 端口）
    bool init();

private:
    // 构建查询报文
    std::vector<uint8_t> build_query(uint16_t id, const std::string &hostname, DNSType type);

    // 解析响应报文
    std::optional<DNSResponse> parse_response(const uint8_t *data, size_t len);

    // 编码域名
    static std::vector<uint8_t> encode_name(const std::string &name);

    // 解码域名（[3]www[6]google[3]com -> www.google.com）
    static std::string decode_name(const uint8_t *data, size_t len, size_t &offset);

private:
    UDPLayer &_udp;
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
};

} // namespace neustack


#endif // NEUSTACK_APP_DNS_CLIENT_HPP