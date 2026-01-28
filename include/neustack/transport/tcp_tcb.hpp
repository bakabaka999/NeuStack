#ifndef NEUSTACK_TRANSPORT_TCP_TCB_HPP
#define NEUSTACK_TRANSPORT_TCP_TCB_HPP

#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>

#include "tcp_state.hpp"

// ========================================================================
// 连接四元组
// ========================================================================

struct TCPTuple {
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    TCPTuple() : local_ip(0), remote_ip(0), local_port(0), remote_port(0) {}

    TCPTuple(uint32_t l_ip, uint32_t r_ip, uint16_t l_p, uint16_t r_p)
        : local_ip(l_ip), remote_ip(r_ip), local_port(l_p), remote_port(r_p) {}

    bool operator==(const TCPTuple &other) const {
        return local_ip == other.local_ip &&
               remote_ip == other.remote_ip &&
               local_port == other.local_port &&
               remote_port == other.remote_port;
    }
};

// 四元组的哈希函数
struct TCPTupleHash {
    size_t operator()(const TCPTuple &t) const {
        // 简单的哈希组合
        size_t h = 0;
        // 采用boost::hash_combine的方法实现
        h ^= std::hash<uint32_t>{}(t.local_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(t.remote_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(t.local_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(t.remote_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ========================================================================
// 回调函数类型
// ========================================================================

struct TCB;  // 前向声明

// 连接建立回调 (tcb: 新建立的连接, error: 0成功, -1失败)
using TCPConnectCallback = std::function<void(TCB* tcb, int error)>;

// 数据接收回调 (tcb: 连接, data: 数据, len: 长度)
using TCPReceiveCallback = std::function<void(TCB* tcb, const uint8_t *data, size_t len)>;

// 连接关闭回调 (tcb: 连接)
using TCPCloseCallback = std::function<void(TCB* tcb)>;

// ========================================================================
// 拥塞控制接口 (为 AI 预留)
// =========================================================================

class ICongestionControl {
public:
    virtual ~ICongestionControl() = default;

    // 收到 ACK 时调用
    virtual void on_ack(uint32_t byte_acked, uint32_t rtt_us) = 0;

    // 检测到丢包时调用
    virtual void on_loss(uint32_t bytes_lost) = 0;

    // 获取拥塞窗口（字节）
    virtual uint32_t cwnd() const = 0;

    // 获取慢启动阈值
    virtual uint32_t ssthresh() const = 0;
};

// ========================================================================
// TCB - Transmission Control Block
// ========================================================================
struct TCB {
    // 连接标志
    TCPTuple t_tuple;
    TCPState state = TCPState::CLOSED;
    bool passive_open = false;  // true = 从 LISTEN 创建（被动打开），false = 从 connect() 创建（主动打开）

    // 发送队列量
    // SND.UNA - 已发送但未确认的最小序列号
    // SND.NXT - 下一个要发送的序列号
    // SND.WND - 发送窗口大小
    // ISS     - 初始发送序列号
    uint32_t snd_una = 0;
    uint32_t snd_nxt = 0;
    uint32_t snd_wnd = 0;
    uint32_t iss = 0;

    // 接收队列量
    // RCV.NXT - 期望接收的下一个序列号
    // RCV.WND - 接收窗口大小
    // IRS     - 初始接收序列号
    uint32_t rcv_nxt = 0;
    uint32_t rcv_wnd = 65535;
    uint32_t irs = 0;

    // 拥塞控制
    std::unique_ptr<ICongestionControl> congestion_control;

    // RTT 估计（用于重传和拥塞控制）
    uint32_t srtt_us = 0;       // 平滑 RTT
    uint32_t rttvar_us = 0;     // RTT 方差
    uint32_t rto_us = 1000000;  // 重传超时（初始化为1s）

    // 缓冲区
    std::vector<uint8_t> send_buffer; // 待发送数据
    std::vector<uint8_t> recv_buffer; // 已接收数据

    // 回调函数
    TCPConnectCallback on_connect;
    TCPReceiveCallback on_receive;
    TCPCloseCallback   on_close;

    // 时间戳（用于超时）
    std::chrono::steady_clock::time_point last_activity;

    // 辅助方法

    // 生成初始序列号
    static uint32_t generate_isn() {
        // 我们暂时的使用一个基于时间的方式实现
        // 后续，我们可以使用 MD5/SHA-256+密钥 实现更安全的算法
        auto now = std::chrono::steady_clock::now(); // 取当前时间
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count(); // 转化为us单位，取其数
        return static_cast<uint32_t>(us);
    }

    // 计算有效发送窗口：min(cwnd, snd_wnd)
    uint32_t effective_window() const {
        uint32_t cwnd = congestion_control ? congestion_control->cwnd() : 65535;
        return std::min(cwnd, snd_wnd);
    }
};

#endif // NEUSTACK_TRANSPORT_TCP_TCB_HPP