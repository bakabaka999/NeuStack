#ifndef NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP
#define NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP

#include "neustack/transport/tcp_tcb.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_seq.hpp"
#include "neustack/common/ring_buffer.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"

#include <unordered_map>
#include <functional>

namespace neustack {

// ========================================================================
// TCP 发送回调 (由上层提供，用于发送 IP 包)
// ========================================================================

// 参数: dst_ip, tcp_data, tcp_len
using TCPSendCallback = std::function<void(uint32_t, const uint8_t *, size_t)>;

// ========================================================================
// TCP 连接管理器
// ========================================================================

class TCPConnectionManager {
// ═══════════════════════════════════════════════════════════════════
// 公开接口
// ═══════════════════════════════════════════════════════════════════

public:
    // ─── 构造与配置 ───
    explicit TCPConnectionManager(uint32_t local_ip) : _local_ip(local_ip) {}

    void set_send_callback(TCPSendCallback cb) { _send_cb = std::move(cb); }
    void set_default_options(const TCPOptions &opts) { _default_options = opts; }
    const TCPOptions &default_options() const { return _default_options; }

    // AI 指标采集：设置采样缓冲区 (由 TCPLayer 调用)
    void set_metrics_buffer(MetricsBuffer<TCPSample, 1024>* buf) { _metrics_buf = buf; }

    // 检查本地端口是否已被使用
    bool is_port_in_use(uint16_t port) const;

public:
    // ─── 主动操作 (应用层调用) ───

    // 监听端口（被动打开）
    int listen(uint16_t port, TCPConnectCallback on_accept);

    // 停止监听端口
    void unlisten(uint16_t port);

    // 设置监听端口的回调（新连接会继承这些回调）
    void set_listener_callbacks(uint16_t port,
                                TCPReceiveCallback on_receive,
                                TCPCloseCallback on_close);

    // 连接远程主机（主动打开，异步）
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                TCPConnectCallback on_connect);

    // 发送数据
    ssize_t send(TCB *tcb, const uint8_t *data, size_t len);

    // 关闭连接
    int close(TCB *tcb);

public:
    // ─── 被动操作 (协议栈内部调用) ───

    // 处理收到的 TCP 段
    void on_segment_received(const TCPSegment &seg);

    // 定时器触发（需要周期性调用，建议 100ms）
    void on_timer();

// ═══════════════════════════════════════════════════════════════════
// 内部实现
// ═══════════════════════════════════════════════════════════════════

private:
    // ─── TCB 管理 ───
    TCB *find_tcb(const TCPTuple &t_tuple);
    TCB *find_listening_tcb(uint16_t port);
    TCB *create_tcb(const TCPTuple &t_tuple);
    void delete_tcb(TCB *tcb);

private:
    // ─── 发送 ───
    void send_segment(TCB *tcb, uint8_t flags, const uint8_t *data = nullptr, size_t len = 0);
    void raw_send(TCB *tcb, uint8_t flags, uint32_t seq, const uint8_t *data = nullptr, size_t len = 0);
    void send_rst(const TCPSegment &seg);

private:
    // ─── 状态机处理 ───
    void handle_closed(TCB *tcb, const TCPSegment &seg);
    void handle_listen(TCB *tcb, const TCPSegment &seg);
    void handle_syn_sent(TCB *tcb, const TCPSegment &seg);
    void handle_syn_rcvd(TCB *tcb, const TCPSegment &seg);
    void handle_established(TCB *tcb, const TCPSegment &seg);
    void handle_fin_wait_1(TCB *tcb, const TCPSegment &seg);
    void handle_fin_wait_2(TCB *tcb, const TCPSegment &seg);
    void handle_close_wait(TCB *tcb, const TCPSegment &seg);
    void handle_closing(TCB *tcb, const TCPSegment &seg);
    void handle_last_ack(TCB *tcb, const TCPSegment &seg);
    void handle_time_wait(TCB *tcb, const TCPSegment &seg);
    void handle_rst(TCB *tcb, const TCPSegment &seg);

private:
    // ─── 定时器 ───
    void start_time_wait_timer(TCB *tcb);
    void restart_time_wait_timer(TCB *tcb);

private:
    // ─── 可靠传输: 重传 ───
    void process_ack(TCB *tcb, uint32_t ack_num, uint16_t window);
    void update_rtt(TCB *tcb, uint32_t rtt_us);
    void check_retransmit(TCB *tcb, std::chrono::steady_clock::time_point now);
    void check_dup_ack(TCB *tcb, uint32_t ack_num);

private:
    // ─── 可靠传输: 乱序处理 ───
    void handle_data(TCB *tcb, const TCPSegment &seg);
    void add_to_ooo_queue(TCB *tcb, uint32_t seq, const uint8_t *data, size_t len);
    void deliver_ooo_data(TCB *tcb);
    void deliver_data(TCB *tcb, const uint8_t *data, size_t len);

private:
    // ─── 流量控制 ───
    void send_buffered_data(TCB *tcb);
    void send_zero_window_probe(TCB *tcb);
    void check_zero_window_probe(TCB *tcb, std::chrono::steady_clock::time_point now);
    void check_delayed_ack(TCB *tcb, std::chrono::steady_clock::time_point now);

// ═══════════════════════════════════════════════════════════════════
// 数据成员
// ═══════════════════════════════════════════════════════════════════

private:
    // ─── 配置 ───
    uint32_t _local_ip;
    TCPSendCallback _send_cb;
    TCPOptions _default_options;

    // ─── AI 指标采集 ───
    MetricsBuffer<TCPSample, 1024>* _metrics_buf = nullptr;

private:
    // ─── 连接表 ───
    std::unordered_map<TCPTuple, std::unique_ptr<TCB>, TCPTupleHash> _connections;
    std::unordered_map<uint16_t, std::unique_ptr<TCB>> _listeners;

private:
    // ─── 定时器数据 ───
    std::vector<std::pair<std::chrono::steady_clock::time_point, TCPTuple>> _time_wait_list;
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP