#include "neustack/app/dns_client.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/common/log.hpp"
#include <cstring>
#include <random>

using namespace neustack;

bool DNSClient::init() {
    // 随机选择一个本地端口
    std::random_device rd;
    _local_port = 10000 + (rd() % 50000);

    // 注册 UDP 回调
    auto udp_callback = [this](uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len) { 
        on_receive(src_ip, src_port, data, len); 
    };
    _udp.bind(_local_port, udp_callback);
    _udp.set_error_callback(_local_port, [this](const ICMPErrorInfo &error) {
        on_error(error);
    });

    LOG_INFO(DNS, "DNS client initialized, local port %u", _local_port);
    return true;
}

void DNSClient::resolve_async(const std::string &hostname, DNSCallback callback,
                              DNSType type) {
    uint16_t id = _next_id++;

    // 记录待处理的查询
    PendingQuery query;
    query.id = id;
    query.hostname = hostname;
    query.type = type;
    query.callback = std::move(callback);
    query.send_time = std::chrono::steady_clock::now();

    _pending[id] = std::move(query);

    // 构建并发送查询
    auto packet = build_query(id, hostname, type);

    _udp.sendto(_dns_server, 53, _local_port, packet.data(), packet.size());

    LOG_DEBUG(DNS, "Query sent: %s (id=%u)", hostname.c_str(), id);
}

void DNSClient::on_receive(uint32_t src_ip, uint16_t src_port,
                           const uint8_t *data, size_t len) {
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
        LOG_DEBUG(DNS, "Duplicate/late response ignored (id=%u)", response->id);
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

    for (auto it = _pending.begin(); it != _pending.end();) {
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
                _udp.sendto(_dns_server, 53, _local_port, packet.data(), packet.size());

                LOG_DEBUG(DNS, "Query retry #%d: %s",
                          it->second.retries, it->second.hostname.c_str());
                ++it;
            }
        } else {
            it++;
        }
    }
}

void DNSClient::on_error(const ICMPErrorInfo &error) {
    if (error.remote_ip != _dns_server || error.remote_port != 53) {
        return;
    }

    LOG_WARN(DNS, "ICMP error for DNS socket: type=%u code=%u from %u.%u.%u.%u",
             static_cast<unsigned>(error.type),
             static_cast<unsigned>(error.code),
             (error.reporter_ip >> 24) & 0xff,
             (error.reporter_ip >> 16) & 0xff,
             (error.reporter_ip >> 8) & 0xff,
             error.reporter_ip & 0xff);

    for (auto &entry : _pending) {
        entry.second.callback(std::nullopt);
    }
    _pending.clear();
}

std::vector<uint8_t> DNSClient::build_query(uint16_t id,
                                            const std::string &hostname,
                                            DNSType type) {
    std::vector<uint8_t> packet;

    // 预留空间
    packet.resize(sizeof(DNSHeader));

    // 填充头部
    auto *hdr = reinterpret_cast<DNSHeader *>(packet.data());
    hdr->id = htons(id);
    hdr->flags = htons(0x0100); // RD=1 (请求递归)
    hdr->qdcount = htons(1);    // 1 个问题
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    // 添加问题区
    auto name = encode_name(hostname);
    packet.insert(packet.end(), name.begin(), name.end());

    // QTYPE
    uint16_t qtype = htons(static_cast<uint16_t>(type));
    packet.push_back(reinterpret_cast<uint8_t *>(&qtype)[0]);
    packet.push_back(reinterpret_cast<uint8_t *>(&qtype)[1]);

    // QCLASS（IN）
    uint16_t qclass = htons(static_cast<uint16_t>(DNSClass::INET));
    packet.push_back(reinterpret_cast<uint8_t *>(&qclass)[0]);
    packet.push_back(reinterpret_cast<uint8_t *>(&qclass)[1]);

    return packet;
}

std::optional<DNSResponse> DNSClient::parse_response(const uint8_t *data, size_t len) {
    if (len < sizeof(DNSHeader)) {
        return std::nullopt;
    }

    auto *hdr = reinterpret_cast<const DNSHeader *>(data);

    DNSResponse response;
    response.id = ntohs(hdr->id);

    uint16_t flags = ntohs(hdr->flags);
    response.rcode = static_cast<DNSRcode>(flags & 0x0f);

    // 检查是否是响应
    if (!(flags & 0x8000)) {
        return std::nullopt; // 不是响应
    }

    uint16_t qdcount = ntohs(hdr->qdcount);
    uint16_t ancount = ntohs(hdr->ancount);

    size_t offset = sizeof(DNSHeader);

    // 跳过问题区
    for (uint16_t i = 0; i < qdcount && offset < len; i++) {
        decode_name(data, len, offset); // 跳过 QNAME
        offset += 4;                    // 跳过 QTYPE 和 QCLASS
    }

    // 解析回答区
    for (uint16_t i = 0; i < ancount && offset < len; i++) {
        DNSRecord record;

        record.name = decode_name(data, len, offset);

        if (offset + 10 > len) break;

        // DNS 规定为大端序
        record.type = static_cast<DNSType>((data[offset] << 8) | data[offset + 1]);
        offset += 2;

        record.cls = static_cast<DNSClass>((data[offset] << 8) | data[offset + 1]);
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

std::vector<uint8_t> DNSClient::encode_name(const std::string &name) {
    std::vector<uint8_t> result;

    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('.', start);
        if (end == std::string::npos) {
            end = name.size();
        }

        size_t len = end - start;
        if (len > 63) len = 63; // 标签最大63个字节

        result.push_back(static_cast<uint8_t>(len));
        result.insert(result.end(), name.begin() + start, name.begin() + end);

        start = end + 1;
    }

    result.push_back(0); // 结束标记
    return result;
}

std::string DNSClient::decode_name(const uint8_t *data, size_t len,
                                   size_t &offset) {
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
