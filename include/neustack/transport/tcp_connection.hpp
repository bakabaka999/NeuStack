#ifndef NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP
#define NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP

#include "neustack/transport/tcp_tcb.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_seq.hpp"

#include <unordered_map>
#include <functional>

// ========================================================================
// TCP 发送回调 (由上层提供，用于发送 IP 包)
// ========================================================================

// 参数: dst_ip, tcp_data, tcp_len
using TCPSendCallback = std::function<void(uint32_t, const uint8_t *, size_t)>;

// ========================================================================
// TCP 连接管理器
// ========================================================================

class TCPConnectionManager {
public:
    explicit TCPConnectionManager(uint32_t local_ip) : _local_ip(local_ip) {}

    // 设置发送回调（由 IP 层调用）
    void set_send_callback(TCPSendCallback cb) { _send_cb = std::move(cb); }

    // 主动操作：应用层调用

    // 监听端口（被动打开）
    // 返回: 0 成功, -1 失败
    int listen(uint16_t port, TCPConnectCallback on_accept);

    // 设置监听端口的回调（新连接会继承这些回调）
    void set_listener_callbacks(uint16_t port,
                                TCPReceiveCallback on_receive,
                                TCPCloseCallback on_close);

    // 连接远程主机 (主动打开)
    // 返回: 0 成功 (异步，结果通过回调通知), -1 失败
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                TCPConnectCallback on_connect);

    // 发送数据
    // 返回: 发送的字节数, -1 失败
    ssize_t send(TCB *tcb, const uint8_t *data, size_t len);

    // 关闭连接 (主动关闭)
    int close(TCB *tcb);

    // 被动操作：收到数据包时调用

    // 处理收到的 TCP 段
    void on_segment_received(const TCPSegment &seg);

    // 定时器触发 (需要定期调用，如每 100ms)
    void on_timer();

private:
    // 内部方法

    // 查找 TCB
    TCB *find_tcb(const TCPTuple &t_tuple);
    TCB *find_listening_tcb(uint16_t port);

    // 创建新的 TCB
    TCB *create_tcb(const TCPTuple &t_tuple);

    // 删除 TCB
    void delete_tcb(TCB *tcb);

    // 发送 TCP 段
    void send_segment(TCB *tcb, uint8_t flags, const uint8_t *data = nullptr, size_t len = 0);
    void send_rst(const TCPSegment &seg); // 对无效段发送 RST

    // 状态机处理（状态转移）
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

    // RST 处理
    void handle_rst(TCB *tcb, const TCPSegment &seg);

    // 定时器
    void start_time_wait_timer(TCB *tcb);
    void restart_time_wait_timer(TCB *tcb);

    // 数据成员
    uint32_t _local_ip;
    TCPSendCallback _send_cb;

    // 活跃连接表 (四元组 -> TCB)
    std::unordered_map<TCPTuple, std::unique_ptr<TCB>, TCPTupleHash> _connections;

    // 监听表 (本地端口 -> TCB)
    std::unordered_map<uint16_t, std::unique_ptr<TCB>> _listeners;

    // TIME_WAIT 连接 (需要单独管理以便定时删除)
    std::vector<std::pair<std::chrono::steady_clock::time_point, TCPTuple>> _time_wait_list;
};

#endif // NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP