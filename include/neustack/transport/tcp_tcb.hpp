#ifndef NEUSTACK_TRANSPORT_TCP_TCB_HPP
#define NEUSTACK_TRANSPORT_TCP_TCB_HPP

#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>
#include <list>

#include "neustack/transport/tcp_state.hpp"
#include "neustack/common/isn_generator.hpp"
#include "neustack/common/ring_buffer.hpp"

namespace neustack {

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
// ========================================================================

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
// 重传队列条目
// ========================================================================
struct RetransmitEntry {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point send_time;
    std::chrono::steady_clock::time_point timeout;
    int retransmit_count = 0;

    // 数据长度
    uint32_t length() const { return seq_end - seq_start; }
};

// ========================================================================
// 乱序数据段
// ========================================================================
struct OutOfOrderSegment {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;

    uint32_t length() const { return seq_end - seq_start; }
};

// ========================================================================
// TCP 连接选项
// ========================================================================

struct TCPOptions {
    // Nagle 算法：合并小包，减少网络开销，但增加延迟
    // 低延迟应用应禁用（如游戏、实时通信）
    bool nagle_enabled = true;

    // 延迟 ACK：等待一段时间或收到第二个包再发 ACK
    // 低延迟应用应禁用
    bool delayed_ack_enabled = true;

    // 延迟 ACK 超时（毫秒）
    uint32_t delayed_ack_timeout_ms = 200;

    // 最大段大小（MSS）
    uint16_t mss = 1460;

    // 接收缓冲区大小
    uint32_t recv_buffer_size = 65535;

    // 发送缓冲区大小
    uint32_t send_buffer_size = 65535;

    // 最大重传次数
    int max_retransmit = 5;

    // 初始 RTO（微秒）
    uint32_t initial_rto_us = 1000000;

    // 快速打开（保留，暂未实现）
    bool tcp_fast_open = false;

    // 时间戳选项（保留，暂未实现）
    bool timestamps_enabled = false;

    // 预设配置
    static TCPOptions low_latency() {
        TCPOptions opts;
        opts.nagle_enabled = false;
        opts.delayed_ack_enabled = false;
        return opts;
    }

    static TCPOptions high_throughput() {
        TCPOptions opts;
        opts.nagle_enabled = true;
        opts.delayed_ack_enabled = true;
        opts.recv_buffer_size = 256 * 1024;  // 256KB
        opts.send_buffer_size = 256 * 1024;
        return opts;
    }

    static TCPOptions interactive() {
        TCPOptions opts;
        opts.nagle_enabled = false;
        opts.delayed_ack_enabled = true;
        opts.delayed_ack_timeout_ms = 100;  // 更短的超时
        return opts;
    }
};

// ========================================================================
// TCB - Transmission Control Block
// ========================================================================
struct TCB {
    // ─── 连接选项（可配置）───
    TCPOptions options;

    // ─── 连接标识 ───
    TCPTuple t_tuple;
    TCPState state = TCPState::CLOSED;
    bool passive_open = false;  // true = 从 LISTEN 创建（被动打开），false = 从 connect() 创建（主动打开）

    // ─── 发送变量 ───
    // SND.UNA - 已发送但未确认的最小序列号
    // SND.NXT - 下一个要发送的序列号
    // SND.WND - 发送窗口大小
    // ISS     - 初始发送序列号
    uint32_t snd_una = 0;
    uint32_t snd_nxt = 0;
    uint32_t snd_wnd = 0;
    uint32_t iss = 0;

    // ─── 接收变量 ───
    // RCV.NXT - 期望接收的下一个序列号
    // RCV.WND - 接收窗口大小
    // IRS     - 初始接收序列号
    uint32_t rcv_nxt = 0;
    uint32_t rcv_wnd = 65535;
    uint32_t irs = 0;

    // ─── 拥塞控制 ───
    std::unique_ptr<ICongestionControl> congestion_control;

    // ─── 重传 ───
    std::list<RetransmitEntry> retransmit_queue;

    // RTT 估计（用于重传和拥塞控制）
    uint32_t srtt_us = 0;       // 平滑 RTT
    uint32_t rttvar_us = 0;     // RTT 方差
    uint32_t rto_us = 1000000;  // 重传超时（初始化为1s）
    uint32_t last_rtt_us = 0;   // 最近一次瞬时 RTT 样本（用于采样导出）

    // 是否有未确认的 RTT 测量
    bool rtt_measuring = false;
    uint32_t rtt_seq = 0; // 用于测量的序列号
    std::chrono::steady_clock::time_point rtt_send_time;

    // ─── 乱序处理 ───
    std::list<OutOfOrderSegment> ooo_queue;
    size_t ooo_size = 0;

    // ─── 快速重传 ───
    uint32_t dup_ack_count = 0;
    uint32_t last_ack_num = 0;

    // ─── 缓冲区(改用环形缓冲区) ───
    // std::vector<uint8_t> send_buffer; // 待发送数据
    // std::vector<uint8_t> recv_buffer; // 已接收数据
    StreamBuffer send_buffer;
    StreamBuffer recv_buffer;

    // ─── 回调函数 ───
    TCPConnectCallback on_connect;
    TCPReceiveCallback on_receive;
    TCPCloseCallback   on_close;

    // ─── 时间戳 ───
    std::chrono::steady_clock::time_point last_activity;

    // ─── 零窗口探测 ───
    bool zero_window_probe_needed = false;
    std::chrono::steady_clock::time_point zwp_time;

    // ─── 延迟 ACK ───
    bool delayed_ack_pending = false;
    std::chrono::steady_clock::time_point delayed_ack_time;

    // ─── 关闭相关 ───
    bool close_pending = false;  // 应用层已调用 close()，等缓冲区发完后发 FIN

    // ─── AI 指标采集 ───
    uint32_t min_rtt_us = 0;                  // 历史最小 RTT (基线延迟)
    uint64_t last_sample_time_us = 0;         // 上次采样时间
    uint16_t packets_sent_period = 0;         // 本采样周期发送的包数
    uint16_t packets_lost_period = 0;         // 本采样周期丢失的包数 (重传)
    uint8_t  ecn_ce_period = 0;               // 本采样周期 ECN CE 标记数
    uint32_t bytes_sent_period = 0;           // 本采样周期发送的字节数
    uint32_t bytes_acked_period = 0;          // 本采样周期确认的字节数
    static constexpr uint64_t SAMPLE_INTERVAL_US = 10000;  // 采样间隔 10ms

    // 构造时初始化
    TCB()
        : send_buffer(65536)   // 64KB 发送缓冲区
        , recv_buffer(65536)   // 64KB 接收缓冲区
    {}


    // ─── 辅助方法 ───

    // 生成初始序列号 (RFC 6528 安全版本)
    static uint32_t generate_isn() {
        return ISNGenerator::generate();
    }

    static uint32_t generate_isn(uint32_t local_ip, uint16_t local_port,
                                  uint32_t remote_ip, uint16_t remote_port) {
        return ISNGenerator::generate(local_ip, local_port, remote_ip, remote_port);
    }

    // 计算有效发送窗口：min(cwnd, snd_wnd)
    uint32_t effective_window() const {
        uint32_t cwnd = congestion_control ? congestion_control->cwnd() : 65535;
        return std::min(cwnd, snd_wnd);
    }

    // 计算当前可用接收窗口
    uint32_t available_recv_window() const {
        size_t used = recv_buffer.size();
        return (used < options.recv_buffer_size) ? (options.recv_buffer_size - used) : 0;
    }

    // 应用选项到 TCB
    void apply_options(const TCPOptions& opts) {
        send_buffer = StreamBuffer(opts.send_buffer_size);
        recv_buffer = StreamBuffer(opts.recv_buffer_size);
        options = opts;
        rto_us = opts.initial_rto_us;
        rcv_wnd = opts.recv_buffer_size;
    }
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_TCB_HPP
