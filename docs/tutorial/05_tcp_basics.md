# 05: TCP 基础 - 头部与数据结构

TCP 是用户态协议栈最具挑战性的部分。它需要处理：可靠传输、流量控制、拥塞控制、连接状态管理等。我们将分多个阶段实现。

本章实现 TCP 的基础结构：头部定义、状态枚举、核心数据结构、段解析与构建。

## 1. TCP 概述

### 1.1 TCP vs UDP

| 特性 | TCP | UDP |
|------|-----|-----|
| 连接 | 面向连接 | 无连接 |
| 可靠性 | 保证交付 | 尽力交付 |
| 顺序 | 保证顺序 | 不保证 |
| 流量控制 | 有 | 无 |
| 拥塞控制 | 有 | 无 |
| 头部大小 | 20-60 字节 | 8 字节 |

### 1.2 TCP 头部格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Data |       |C|E|U|A|P|R|S|F|                               |
| Offset| Rsrvd |W|C|R|C|S|S|Y|I|            Window             |
|       |       |R|E|G|K|H|T|N|N|                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options                    |    Padding    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             Data                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 1.3 TCP 标志位

| 标志 | 含义 |
|------|------|
| SYN | 同步序列号，建立连接 |
| ACK | 确认字段有效 |
| FIN | 发送方完成发送 |
| RST | 重置连接 |
| PSH | 推送数据，不要缓冲 |
| URG | 紧急指针有效 |

## 2. TCP 状态机

TCP 有 11 个状态，这是理解 TCP 的关键：

```
                              +---------+ ---------\      active OPEN
                              |  CLOSED |            \    -----------
                              +---------+<---------\   \   create TCB
                                |     ^              \   \  snd SYN
                   passive OPEN |     |   CLOSE        \   \
                   ------------ |     | ----------       \   \
                    create TCB  |     | delete TCB         \   \
                                V     |                      \   \
                              +---------+            CLOSE    |    \
                              |  LISTEN |          ---------- |     |
                              +---------+          delete TCB |     |
                   rcv SYN      |     |     SEND              |     |
                   -------      |     |    -------            |     V
  +---------+      snd SYN,ACK  /       \   snd SYN          +---------+
  |         |<-----------------           ------------------>|         |
  |   SYN   |                    rcv SYN                     |   SYN   |
  |   RCVD  |<-----------------------------------------------|   SENT  |
  |         |                    snd ACK                     |         |
  |         |------------------           -------------------|         |
  +---------+   rcv ACK of SYN  \       /  rcv SYN,ACK       +---------+
      |           -------------- |     | -----------
      |           x              |     |   snd ACK
      |                          V     V
      |                        +---------+
      |                        |  ESTAB  |
      |                        +---------+
      |                 CLOSE    |     |    rcv FIN
      |                -------   |     |    -------
      V                snd FIN  /       \   snd ACK          +---------+
  +---------+                  /         \                   |  CLOSE  |
  |  FIN    |<----------------           ------------------>|   WAIT  |
  | WAIT-1  |------------------                              +---------+
  +---------+          rcv FIN  \                                 |
    | rcv ACK of FIN   -------   |                                |
    | --------------   snd ACK   |                                V
    V        x                   V                            +---------+
  +---------+                  +---------+                    |LAST-ACK |
  |FINWAIT-2|                  | CLOSING |                    +---------+
  +---------+                  +---------+                        |
    |                rcv ACK of FIN |                             | rcv ACK of FIN
    |  rcv FIN       -------------- |                             | --------------
    |  -------              x       V                             V        x
     \ snd ACK                 +---------+                    +---------+
      ------------------------>|TIME WAIT|-------------------->| CLOSED  |
                               +---------+   Timeout=2MSL     +---------+
```

### 2.1 状态说明

| 状态 | 说明 |
|------|------|
| CLOSED | 初始状态，无连接 |
| LISTEN | 服务端等待连接 |
| SYN_SENT | 客户端已发送 SYN |
| SYN_RCVD | 服务端收到 SYN，已发送 SYN+ACK |
| ESTABLISHED | 连接建立，可以传输数据 |
| FIN_WAIT_1 | 主动关闭，已发送 FIN |
| FIN_WAIT_2 | 主动关闭，已收到对方的 ACK |
| CLOSE_WAIT | 被动关闭，收到 FIN，等待应用关闭 |
| CLOSING | 双方同时关闭 |
| LAST_ACK | 被动关闭，等待最后的 ACK |
| TIME_WAIT | 等待 2MSL 后关闭（确保对方收到 ACK） |

## 3. 数据结构设计

### 3.1 TCP 头部

```cpp
// include/neustack/transport/tcp.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_HPP
#define NEUSTACK_TRANSPORT_TCP_HPP

#include <cstdint>
#include <arpa/inet.h>

// ============================================================================
// TCP 标志位
// ============================================================================

namespace TCPFlags {
    constexpr uint8_t FIN = 0x01;
    constexpr uint8_t SYN = 0x02;
    constexpr uint8_t RST = 0x04;
    constexpr uint8_t PSH = 0x08;
    constexpr uint8_t ACK = 0x10;
    constexpr uint8_t URG = 0x20;
    constexpr uint8_t ECE = 0x40;
    constexpr uint8_t CWR = 0x80;
}

// ============================================================================
// TCP 头部 (网络字节序)
// ============================================================================

struct TCPHeader {
    uint16_t src_port;      // 源端口
    uint16_t dst_port;      // 目标端口
    uint32_t seq_num;       // 序列号
    uint32_t ack_num;       // 确认号
    uint8_t  data_offset;   // 数据偏移 (高4位) + 保留 (低4位)
    uint8_t  flags;         // 标志位
    uint16_t window;        // 窗口大小
    uint16_t checksum;      // 校验和
    uint16_t urgent_ptr;    // 紧急指针

    // 辅助方法
    uint8_t header_length() const {
        return (data_offset >> 4) * 4;
    }

    bool has_flag(uint8_t flag) const {
        return (flags & flag) != 0;
    }
};

static_assert(sizeof(TCPHeader) == 20, "TCPHeader must be 20 bytes");

// ============================================================================
// TCP 伪头部 (用于校验和)
// ============================================================================

struct TCPPseudoHeader {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint8_t  zero;
    uint8_t  protocol;      // 6 = TCP
    uint16_t tcp_length;
};

static_assert(sizeof(TCPPseudoHeader) == 12, "TCPPseudoHeader must be 12 bytes");

#endif // NEUSTACK_TRANSPORT_TCP_HPP
```

### 3.2 TCP 状态

```cpp
// include/neustack/transport/tcp_state.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_STATE_HPP
#define NEUSTACK_TRANSPORT_TCP_STATE_HPP

#include <cstdint>

// ============================================================================
// TCP 状态
// ============================================================================

enum class TCPState : uint8_t {
    CLOSED = 0,
    LISTEN,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    CLOSING,
    LAST_ACK,
    TIME_WAIT
};

// 状态名称（用于调试）
inline const char* tcp_state_name(TCPState state) {
    switch (state) {
        case TCPState::CLOSED:      return "CLOSED";
        case TCPState::LISTEN:      return "LISTEN";
        case TCPState::SYN_SENT:    return "SYN_SENT";
        case TCPState::SYN_RCVD:    return "SYN_RCVD";
        case TCPState::ESTABLISHED: return "ESTABLISHED";
        case TCPState::FIN_WAIT_1:  return "FIN_WAIT_1";
        case TCPState::FIN_WAIT_2:  return "FIN_WAIT_2";
        case TCPState::CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCPState::CLOSING:     return "CLOSING";
        case TCPState::LAST_ACK:    return "LAST_ACK";
        case TCPState::TIME_WAIT:   return "TIME_WAIT";
        default:                    return "UNKNOWN";
    }
}

#endif // NEUSTACK_TRANSPORT_TCP_STATE_HPP
```

### 3.3 TCB (Transmission Control Block)

TCB 是每个 TCP 连接的核心数据结构：

```cpp
// include/neustack/transport/tcp_tcb.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_TCB_HPP
#define NEUSTACK_TRANSPORT_TCP_TCB_HPP

#include "tcp_state.hpp"
#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>

// ============================================================================
// 连接四元组
// ============================================================================

struct TCPTuple {
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    bool operator==(const TCPTuple& other) const {
        return local_ip == other.local_ip &&
               remote_ip == other.remote_ip &&
               local_port == other.local_port &&
               remote_port == other.remote_port;
    }
};

// 用于 unordered_map 的哈希函数
struct TCPTupleHash {
    size_t operator()(const TCPTuple& t) const {
        // 简单的哈希组合
        size_t h = 0;
        h ^= std::hash<uint32_t>{}(t.local_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(t.remote_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(t.local_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(t.remote_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ============================================================================
// 回调函数类型
// ============================================================================

// 连接建立回调
using TCPConnectCallback = std::function<void(int error)>;

// 数据接收回调
using TCPReceiveCallback = std::function<void(const uint8_t* data, size_t len)>;

// 连接关闭回调
using TCPCloseCallback = std::function<void()>;

// ============================================================================
// 拥塞控制接口 (为 AI 预留)
// ============================================================================

class ICongestionControl {
public:
    virtual ~ICongestionControl() = default;

    // 收到 ACK 时调用
    virtual void on_ack(uint32_t bytes_acked, uint32_t rtt_us) = 0;

    // 检测到丢包时调用
    virtual void on_loss(uint32_t bytes_lost) = 0;

    // 获取拥塞窗口 (字节)
    virtual uint32_t cwnd() const = 0;

    // 获取慢启动阈值
    virtual uint32_t ssthresh() const = 0;
};

// ============================================================================
// TCB - Transmission Control Block
// ============================================================================

struct TCB {
    // ─── 连接标识 ───
    TCPTuple tuple;
    TCPState state = TCPState::CLOSED;

    // ─── 发送序列号空间 ───
    // SND.UNA - 已发送但未确认的最小序列号
    // SND.NXT - 下一个要发送的序列号
    // SND.WND - 发送窗口大小
    // ISS     - 初始发送序列号
    uint32_t snd_una = 0;   // 最早未确认的序列号
    uint32_t snd_nxt = 0;   // 下一个发送的序列号
    uint32_t snd_wnd = 0;   // 发送窗口 (对方通告的)
    uint32_t iss = 0;       // 初始发送序列号

    // ─── 接收序列号空间 ───
    // RCV.NXT - 期望接收的下一个序列号
    // RCV.WND - 接收窗口大小
    // IRS     - 初始接收序列号
    uint32_t rcv_nxt = 0;   // 期望接收的序列号
    uint32_t rcv_wnd = 65535; // 接收窗口大小 (我们通告给对方的)
    uint32_t irs = 0;       // 初始接收序列号

    // ─── 拥塞控制 ───
    std::unique_ptr<ICongestionControl> congestion_control;

    // ─── RTT 估计 (用于重传和拥塞控制) ───
    uint32_t srtt_us = 0;       // 平滑 RTT (微秒)
    uint32_t rttvar_us = 0;     // RTT 方差
    uint32_t rto_us = 1000000;  // 重传超时 (初始 1 秒)

    // ─── 缓冲区 ───
    std::vector<uint8_t> send_buffer;   // 待发送数据
    std::vector<uint8_t> recv_buffer;   // 已接收数据

    // ─── 回调 ───
    TCPConnectCallback on_connect;
    TCPReceiveCallback on_receive;
    TCPCloseCallback on_close;

    // ─── 时间戳 (用于超时) ───
    std::chrono::steady_clock::time_point last_activity;

    // ─── 辅助方法 ───

    // 生成初始序列号
    static uint32_t generate_isn() {
        // 简单实现：基于时间
        // 实际应该更随机以防止攻击
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        return static_cast<uint32_t>(us);
    }

    // 计算有效发送窗口 = min(cwnd, snd_wnd)
    uint32_t effective_window() const {
        uint32_t cwnd = congestion_control ? congestion_control->cwnd() : 65535;
        return std::min(cwnd, snd_wnd);
    }
};

#endif // NEUSTACK_TRANSPORT_TCP_TCB_HPP
```

## 4. TCP 段解析

```cpp
// include/neustack/transport/tcp_segment.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP
#define NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP

#include "tcp.hpp"
#include "neustack/net/ipv4.hpp"
#include <optional>

// ============================================================================
// 解析后的 TCP 段
// ============================================================================

struct TCPSegment {
    // 来源信息 (从 IP 层)
    uint32_t src_addr;
    uint32_t dst_addr;

    // TCP 头部字段 (主机字节序)
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   // 头部长度 (字节)
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;

    // 数据
    const uint8_t* data;
    size_t data_length;

    // 便捷方法
    bool is_syn() const { return flags & TCPFlags::SYN; }
    bool is_ack() const { return flags & TCPFlags::ACK; }
    bool is_fin() const { return flags & TCPFlags::FIN; }
    bool is_rst() const { return flags & TCPFlags::RST; }
    bool is_psh() const { return flags & TCPFlags::PSH; }

    // 序列号计算
    // SYN 和 FIN 各占一个序列号
    uint32_t seg_len() const {
        uint32_t len = data_length;
        if (is_syn()) len++;
        if (is_fin()) len++;
        return len;
    }

    // 段的结束序列号 (不含)
    uint32_t seq_end() const {
        return seq_num + seg_len();
    }
};

// ============================================================================
// TCP 解析器
// ============================================================================

class TCPParser {
public:
    // 从 IPv4 包解析 TCP 段
    static std::optional<TCPSegment> parse(const IPv4Packet& pkt);

    // 验证校验和
    static bool verify_checksum(const IPv4Packet& pkt);

private:
    static uint16_t compute_tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t* tcp_data, size_t tcp_len);
};

#endif // NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP
```

## 5. 序列号比较

TCP 序列号是 32 位，会回绕。比较时需要特殊处理：

```cpp
// include/neustack/transport/tcp_seq.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_SEQ_HPP
#define NEUSTACK_TRANSPORT_TCP_SEQ_HPP

#include <cstdint>

// ============================================================================
// TCP 序列号比较 (处理回绕)
// ============================================================================

// 序列号使用模 2^32 算术，比较时使用有符号差值

// a < b
inline bool seq_lt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

// a <= b
inline bool seq_le(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) <= 0;
}

// a > b
inline bool seq_gt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

// a >= b
inline bool seq_ge(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) >= 0;
}

// 检查 seq 是否在 [start, end) 范围内
inline bool seq_in_range(uint32_t seq, uint32_t start, uint32_t end) {
    return seq_ge(seq, start) && seq_lt(seq, end);
}

#endif // NEUSTACK_TRANSPORT_TCP_SEQ_HPP
```

## 6. TCP 构建器

```cpp
// include/neustack/transport/tcp_builder.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_BUILDER_HPP
#define NEUSTACK_TRANSPORT_TCP_BUILDER_HPP

#include "tcp.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================================
// TCP 段构建器
// ============================================================================

class TCPBuilder {
public:
    TCPBuilder& set_src_port(uint16_t port) { _src_port = port; return *this; }
    TCPBuilder& set_dst_port(uint16_t port) { _dst_port = port; return *this; }
    TCPBuilder& set_seq(uint32_t seq) { _seq = seq; return *this; }
    TCPBuilder& set_ack(uint32_t ack) { _ack = ack; return *this; }
    TCPBuilder& set_flags(uint8_t flags) { _flags = flags; return *this; }
    TCPBuilder& set_window(uint16_t window) { _window = window; return *this; }
    TCPBuilder& set_payload(const uint8_t* data, size_t len) {
        _payload = data;
        _payload_len = len;
        return *this;
    }

    // 构建 TCP 段 (不含校验和，由调用者计算)
    // 返回构建的长度，-1 表示失败
    ssize_t build(uint8_t* buffer, size_t buffer_len) const;

    // 计算并填充校验和
    static void fill_checksum(uint8_t* tcp_data, size_t tcp_len,
                              uint32_t src_ip, uint32_t dst_ip);

private:
    uint16_t _src_port = 0;
    uint16_t _dst_port = 0;
    uint32_t _seq = 0;
    uint32_t _ack = 0;
    uint8_t  _flags = 0;
    uint16_t _window = 65535;
    const uint8_t* _payload = nullptr;
    size_t _payload_len = 0;
};

#endif // NEUSTACK_TRANSPORT_TCP_BUILDER_HPP
```

## 7. 练习

1. **实现 TCP 头部解析** ✓
   - 创建 `src/transport/tcp_segment.cpp`
   - 实现 `TCPParser::parse()` 和校验和验证

2. **实现 TCP 段构建器** ✓
   - 创建 `src/transport/tcp_builder.cpp`
   - 实现 `TCPBuilder::build()` 和 `fill_checksum()`

## 8. 关键点总结

1. **TCP 头部固定 20 字节**：可选项最多 40 字节
2. **序列号会回绕**：使用专门的比较函数 (`seq_lt`, `seq_gt` 等)
3. **SYN 和 FIN 各占一个序列号**：计算 `seg_len()` 时要考虑
4. **校验和包含伪头部**：和 UDP 类似，但 protocol = 6
5. **TCB 是核心**：每个连接一个 TCB，存储所有状态
6. **拥塞控制接口已预留**：`ICongestionControl` 接口支持后续接入 AI 算法

## 9. 已完成的文件

```
include/neustack/transport/
├── tcp.hpp              ✓ (TCPHeader, TCPFlags, TCPPseudoHeader)
├── tcp_state.hpp        ✓ (TCPState 枚举)
├── tcp_tcb.hpp          ✓ (TCB, TCPTuple, ICongestionControl)
├── tcp_seq.hpp          ✓ (序列号比较函数)
├── tcp_segment.hpp      ✓ (TCPSegment, TCPParser)
└── tcp_builder.hpp      ✓ (TCPBuilder)

src/transport/
├── tcp_segment.cpp      ✓ (解析和校验和)
└── tcp_builder.cpp      ✓ (构建和校验和)
```

## 10. 下一步

下一章 (06) 我们将实现 TCP 连接管理：
- TCPConnectionManager 类
- 三次握手（主动和被动）
- 四次挥手
- RST 处理
- 状态机完整实现
