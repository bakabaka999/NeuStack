#include "neustack/transport/tcp_connection.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/transport/tcp_reno.hpp"
#include "neustack/transport/tcp_cubic.hpp"
#include "neustack/transport/tcp_orca.hpp"
#include "neustack/telemetry/metrics_registry.hpp"

using namespace neustack;

constexpr auto TIME_WAIT_DURATION = std::chrono::seconds(32);
constexpr size_t MAX_CONNECTIONS = 10000;
constexpr size_t MAX_TIME_WAIT_ENTRIES = 4000;

int TCPConnectionManager::listen(uint16_t port, TCPConnectCallback on_accept) {
    // 检查端口合法性 (0-1024 通常需要特权，虽然在我们的 stack 里可以随便用)
    if (port == 0) {
        LOG_ERROR(TCP, "Cannot listen on port 0");
        return -1;
    }
    // 检查该端口是否已经被监听或占用
    if (_listeners.count(port)) {
        LOG_WARN(TCP, "Port %u is already being listened on", port);
        return -1;
    }

    // 创建一个特殊的 "Listening TCB"
    // 监听状态的 TCB 不需要完整的四元组，只需要 local_port
    auto tcb = std::make_unique<TCB>();

    tcb->state = TCPState::LISTEN;
    tcb->t_tuple = TCPTuple(_local_ip, 0, port, 0);
    tcb->on_connect = std::move(on_accept); // 握手成功后的回调

    // 注册到监听表
    _listeners[port] = std::move(tcb);

    LOG_INFO(TCP, "Server is now listening on port %u", port);
    return 0;
}

void TCPConnectionManager::set_listener_callbacks(uint16_t port,
                                                   TCPReceiveCallback on_receive,
                                                   TCPCloseCallback on_close) {
    auto it = _listeners.find(port);
    if (it != _listeners.end()) {
        it->second->on_receive = std::move(on_receive);
        it->second->on_close = std::move(on_close);
    }
}

void TCPConnectionManager::unlisten(uint16_t port) {
    auto it = _listeners.find(port);
    if (it != _listeners.end()) {
        LOG_INFO(TCP, "Stopped listening on port %u", port);
        _listeners.erase(it);
    }
}

int TCPConnectionManager::connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, TCPConnectCallback on_connect) {
    // 构造四元组
    TCPTuple tuple{_local_ip, remote_ip, local_port, remote_port};

    // 连接数上限检查
    if (_connections.size() >= MAX_CONNECTIONS) {
        LOG_WARN(TCP, "Connection limit reached (%zu), cannot connect", MAX_CONNECTIONS);
        return -1;
    }

    // 检查连接是否已存在
    if (find_tcb(tuple)) {
        LOG_WARN(TCP, "Connect failed: connection already exists for %s:%u -> %s:%u",
                 ip_to_string(tuple.local_ip).c_str(), tuple.local_port,
                 ip_to_string(tuple.remote_ip).c_str(), tuple.remote_port);
        return -1;
    }

    // 创建 TCB
    TCB *tcb = create_tcb(tuple);
    tcb->on_connect = std::move(on_connect);

    // 生成初始序列号（ISN）- 使用四元组确保安全性
    tcb->iss = TCB::generate_isn(tuple.local_ip, tuple.local_port,
                                  tuple.remote_ip, tuple.remote_port);
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;  // send_segment 会在发送 SYN 时递增

    // 发送 SYN（send_segment 内部 snd_nxt += 1）
    send_segment(tcb, TCPFlags::SYN);

    // 进入 SYN_SENT 状态
    tcb->state = TCPState::SYN_SENT;

    LOG_INFO(TCP, "CLOSED -> SYN_SENT: connecting to %s:%u",
             ip_to_string(remote_ip).c_str(), remote_port);

    return 0;
}

ssize_t TCPConnectionManager::send(TCB *tcb, const uint8_t *data, size_t len) {
    // 只有在 ESTABLISHED 或 CLOSE_WAIT 状态才能发送数据
    if (tcb->state != TCPState::ESTABLISHED && tcb->state != TCPState::CLOSE_WAIT) {
        LOG_WARN(TCP, "Cannot send data in state %s", tcp_state_name(tcb->state));
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    // 检查发送缓冲区是否有空间
    size_t buffer_space = tcb->send_buffer.available();
    if (buffer_space == 0) {
        LOG_DEBUG(TCP, "Send buffer full, cannot accept more data");
        return -1;  // 缓冲区已满，无法接受更多数据
    }

    // 限制为可用缓冲空间
    size_t accepted = std::min(len, buffer_space);

    uint16_t mss = tcb->options.mss;

    // 关键：如果 send_buffer 已经有数据在排队，新数据必须追加到队尾
    // 否则会导致乱序发送！
    if (!tcb->send_buffer.empty()) {
        tcb->send_buffer.write(data, accepted);
        LOG_DEBUG(TCP, "Appended %zu bytes to existing buffer (%zu total)",
                  accepted, tcb->send_buffer.size());
        return static_cast<ssize_t>(accepted);
    }

    // Nagle 算法：如果有未确认的数据，且当前数据小于 MSS，就缓冲
    if (tcb->options.nagle_enabled &&
        tcb->snd_nxt != tcb->snd_una &&  // 有未确认数据
        accepted < mss) {                 // 当前数据小于 MSS

        // 缓冲数据，等待 ACK 或凑够 MSS
        tcb->send_buffer.write(data, accepted);
        LOG_DEBUG(TCP, "Nagle: buffered %zu bytes", accepted);
        return static_cast<ssize_t>(accepted);
    }


    // 计算可发送的数据量
    // 受限于：1) 发送窗口 2) 拥塞窗口
    uint32_t window = tcb->effective_window();
    uint32_t in_flight = tcb->snd_nxt - tcb->snd_una; // 已发送未确认的数据量
    uint32_t available = (window > in_flight) ? (window - in_flight) : 0;

    if (available == 0) {
        // 窗口已满，将数据放入发送缓冲区等待
        tcb->send_buffer.write(data, accepted);
        LOG_DEBUG(TCP, "Window full, buffered %zu bytes", accepted);
        return static_cast<ssize_t>(accepted);
    }

    // 限制单次发送的大小
    size_t to_send = std::min({accepted, static_cast<size_t>(available), static_cast<size_t>(mss)});

    // 发送数据段
    send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH, data, to_send);

    // 剩余数据放入缓冲区
    if (to_send < accepted) {
        tcb->send_buffer.write(data + to_send, accepted - to_send);
    }

    LOG_DEBUG(TCP, "Sent %zu bytes, buffered %zu bytes", to_send, accepted - to_send);

    // 关键：初始发送后，继续发送缓冲区中的数据直到填满窗口
    // 否则只发 1 个 MSS，剩余要等 ACK 才能发，浪费 cwnd
    if (!tcb->send_buffer.empty()) {
        send_buffered_data(tcb);
    }

    return static_cast<ssize_t>(accepted);
}

int TCPConnectionManager::close(TCB *tcb) {
    switch (tcb->state) {
        case TCPState::ESTABLISHED:
            // 先发完缓冲区中的数据
            send_buffered_data(tcb);

            // 如果缓冲区还有数据（窗口满了），标记需要关闭，等数据发完再发 FIN
            if (!tcb->send_buffer.empty()) {
                tcb->close_pending = true;
                LOG_DEBUG(TCP, "Close pending: %zu bytes still in buffer", tcb->send_buffer.size());
                return 0;
            }

            // 发送 FIN，告诉对方我们不再发送数据
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->state = TCPState::FIN_WAIT_1;
            LOG_INFO(TCP, "ESTABLISHED -> FIN_WAIT_1: initiating close");
            break;

        case TCPState::CLOSE_WAIT:
            // 已经收到了对方的 FIN
            // 先发完缓冲区中的数据
            send_buffered_data(tcb);

            // 如果缓冲区还有数据，标记需要关闭
            if (!tcb->send_buffer.empty()) {
                tcb->close_pending = true;
                LOG_DEBUG(TCP, "Close pending: %zu bytes still in buffer", tcb->send_buffer.size());
                return 0;
            }

            // 应用层处理完后调用 close，现在发送 FIN
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->state = TCPState::LAST_ACK;
            LOG_INFO(TCP, "CLOSE_WAIT -> LAST_ACK: sending FIN");
            break;

        case TCPState::LISTEN:
            LOG_INFO(TCP, "LISTEN -> CLOSED: stop listening");
            delete_tcb(tcb);
            break;

        case TCPState::SYN_SENT:
            LOG_INFO(TCP, "SYN_SENT -> CLOSED: aborting connection");
            delete_tcb(tcb);
            break;

        default:
            LOG_WARN(TCP, "close() called in unexpected state %s", tcp_state_name(tcb->state));
            break;
    }
    return 0;
}

void TCPConnectionManager::on_segment_received(const TCPSegment &seg) {
    // ─── AI 指标采集: 全局统计 ───
    global_metrics().packets_rx.fetch_add(1, std::memory_order_relaxed);
    global_metrics().bytes_rx.fetch_add(seg.data_length + 20, std::memory_order_relaxed);  // +20 for TCP header
    if (seg.is_syn() && !seg.is_ack()) {
        global_metrics().syn_received.fetch_add(1, std::memory_order_relaxed);
    }
    if (seg.is_rst()) {
        global_metrics().rst_received.fetch_add(1, std::memory_order_relaxed);
    }
    if (seg.is_fin()) {
        global_metrics().fin_received.fetch_add(1, std::memory_order_relaxed);
    }

    LOG_DEBUG(TCP, "Received: %s:%u -> %s:%u [%s%s%s%s%s] seq=%u ack=%u len=%zu win=%u",
              ip_to_string(seg.src_addr).c_str(), seg.src_port,
              ip_to_string(seg.dst_addr).c_str(), seg.dst_port,
              seg.is_syn() ? "SYN " : "",
              seg.is_ack() ? "ACK " : "",
              seg.is_fin() ? "FIN " : "",
              seg.is_rst() ? "RST " : "",
              seg.is_psh() ? "PSH " : "",
              seg.seq_num, seg.ack_num, seg.data_length, seg.window);

    // 查找对应的 TCB
    TCPTuple t_tuple{seg.dst_addr, seg.src_addr, seg.dst_port, seg.src_port};
    TCB *tcb = find_tcb(t_tuple);

    // 没有已经建立的连接就检查监听
    if (!tcb) {
        TCB *listen_tcb = find_listening_tcb(seg.dst_port);
        if (listen_tcb) {
            tcb = listen_tcb;
        } else {
            // 没有监听就发送 RST（除非接受的是一个RST
            LOG_DEBUG(TCP, "No TCB found for port %u, sending RST", seg.dst_port);
            if (!seg.is_rst()) {
                send_rst(seg);
            }
            return;
        }
    }

    // 先处理 RST
    if (seg.is_rst()) {
        handle_rst(tcb, seg);
        return;
    }

    // 其他的就根据状态分发
    switch (tcb->state) {
        case TCPState::CLOSED:      handle_closed(tcb, seg);      break;
        case TCPState::LISTEN:      handle_listen(tcb, seg);      break;
        case TCPState::SYN_SENT:    handle_syn_sent(tcb, seg);    break;
        case TCPState::SYN_RCVD:    handle_syn_rcvd(tcb, seg);    break;
        case TCPState::ESTABLISHED: handle_established(tcb, seg); break;
        case TCPState::FIN_WAIT_1:  handle_fin_wait_1(tcb, seg);  break;
        case TCPState::FIN_WAIT_2:  handle_fin_wait_2(tcb, seg);  break;
        case TCPState::CLOSE_WAIT:  handle_close_wait(tcb, seg);  break;
        case TCPState::CLOSING:     handle_closing(tcb, seg);     break;
        case TCPState::LAST_ACK:    handle_last_ack(tcb, seg);    break;
        case TCPState::TIME_WAIT:   handle_time_wait(tcb, seg);   break;
    }
}

void TCPConnectionManager::on_timer() {
    auto now = std::chrono::steady_clock::now();

    // 收集需要删除的连接（超过最大重传次数）
    std::vector<TCPTuple> to_delete;

    // 检查所有连接的重传队列
    for (auto& [tuple, tcb] : _connections) {
        check_retransmit(tcb.get(), now);

        // 如果连接已经标记为 CLOSED（超过最大重传），加入删除列表
        if (tcb->state == TCPState::CLOSED) {
            to_delete.push_back(tuple);
            continue;
        }

        // 检查delayed_ack
        check_delayed_ack(tcb.get(), now);
        // 检查零窗口探测
        check_zero_window_probe(tcb.get(), now);
    }

    // 删除已关闭的连接
    for (const auto& tuple : to_delete) {
        _connections.erase(tuple);
    }

    // 处理 TIME_WAIT 超时
    for (auto it = _time_wait_list.begin(); it != _time_wait_list.end();) {
        if (now >= it->first) {
            TCB *tcb = find_tcb(it->second);
            if (tcb && tcb->state == TCPState::TIME_WAIT) {
                tcb->state = TCPState::CLOSED;
                delete_tcb(tcb);
            }
            it = _time_wait_list.erase(it);
        } else {
            it++;
        }
    }
}

bool TCPConnectionManager::is_port_in_use(uint16_t port) const {
    // 检查监听表
    if (_listeners.count(port)) {
        return true;
    }
    // 检查连接表中是否有使用该本地端口的连接
    for (const auto& [tuple, tcb] : _connections) {
        if (tuple.local_port == port) {
            return true;
        }
    }
    return false;
}

TCB *TCPConnectionManager::find_tcb(const TCPTuple &t_tuple) {
    auto it = _connections.find(t_tuple);
    if (it != _connections.end()) {
        return it->second.get();
    }
    return nullptr;
}

TCB *TCPConnectionManager::find_listening_tcb(uint16_t port) {
    auto it = _listeners.find(port);
    if (it != _listeners.end()) {
        return it->second.get();
    }
    return nullptr;
}

TCB *TCPConnectionManager::create_tcb(const TCPTuple &t_tuple) {
    auto tcb = std::make_unique<TCB>();
    tcb->t_tuple = t_tuple;
    tcb->state = TCPState::CLOSED;
    tcb->last_activity = std::chrono::steady_clock::now();

    // 应用默认选项
    tcb->apply_options(_default_options);

    // 创建拥塞控制器
#ifdef NEUSTACK_AI_ENABLED
    tcb->congestion_control = std::make_unique<TCPOrca>(tcb->options.mss);
#else
    tcb->congestion_control = std::make_unique<TCPCubic>(tcb->options.mss);
#endif

    TCB *ptr = tcb.get();
    _connections[t_tuple] = std::move(tcb);

    LOG_DEBUG(TCP, "Created TCB for %s:%u <-> %s:%u",
              ip_to_string(t_tuple.local_ip).c_str(), t_tuple.local_port,
              ip_to_string(t_tuple.remote_ip).c_str(), t_tuple.remote_port);

    return ptr;
}

void TCPConnectionManager::delete_tcb(TCB *tcb) {
    if (!tcb) return;

    // 先尝试从监听表删除
    for (auto it = _listeners.begin(); it != _listeners.end(); ++it) {
        if (it->second.get() == tcb) {
            LOG_DEBUG(TCP, "Deleted listening TCB on port %u", it->first);
            _listeners.erase(it);
            return;
        }
    }

    // 再从连接表删除
    auto it = _connections.find(tcb->t_tuple);
    if (it != _connections.end()) {
        // ─── AI 指标采集: 连接关闭 ───
        // 只有曾经 ESTABLISHED 的连接才计入 active_connections
        if (tcb->state == TCPState::ESTABLISHED ||
            tcb->state == TCPState::FIN_WAIT_1 ||
            tcb->state == TCPState::FIN_WAIT_2 ||
            tcb->state == TCPState::CLOSE_WAIT ||
            tcb->state == TCPState::CLOSING ||
            tcb->state == TCPState::LAST_ACK ||
            tcb->state == TCPState::TIME_WAIT) {
            global_metrics().active_connections.fetch_sub(1, std::memory_order_relaxed);
        }
        global_metrics().conn_closed.fetch_add(1, std::memory_order_relaxed);

        LOG_DEBUG(TCP, "Deleted TCB for %s:%u <-> %s:%u",
                  ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
                  ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port);
        _connections.erase(it);
    }
}

void TCPConnectionManager::send_segment(TCB *tcb, uint8_t flags, const uint8_t *data, size_t len) {
    constexpr size_t MAX_TCP_SIZE = 1500 - 20 - 20; // MTU - IP头 - TCP头
    uint8_t buffer[MAX_TCP_SIZE + sizeof(TCPHeader)];

    // 验证数据长度不超过缓冲区大小
    if (len > MAX_TCP_SIZE) {
        LOG_ERROR(TCP, "Data too large for TCP segment: %zu > %zu", len, MAX_TCP_SIZE);
        return;
    }

    // 记录发送前的序列号（用于重传队列）
    // SYN 时用 snd_una（ISS），否则用 snd_nxt
    uint32_t seq_used = (flags & TCPFlags::SYN) ? tcb->snd_una : tcb->snd_nxt;

    TCPBuilder builder;
    builder.set_src_port(tcb->t_tuple.local_port)
           .set_dst_port(tcb->t_tuple.remote_port)
           .set_seq(seq_used)
           .set_ack(tcb->rcv_nxt)
           .set_flags(flags)
           .set_window(static_cast<uint16_t>(std::min(tcb->rcv_wnd, 65535u)));

    if (data && len > 0) {
        builder.set_payload(data, len);
    }

    ssize_t tcp_len = builder.build(buffer, sizeof(buffer));
    if (tcp_len < 0) {
        LOG_ERROR(TCP, "Failed to build TCP segment");
        return;
    }

    // 填充校验和
    TCPBuilder::fill_checksum(buffer, tcp_len,
                              tcb->t_tuple.local_ip, tcb->t_tuple.remote_ip);

    LOG_DEBUG(TCP, "Sending: %s:%u -> %s:%u [%s%s%s%s] seq=%u ack=%u len=%zu win=%u",
              ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
              ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port,
              (flags & TCPFlags::SYN) ? "SYN " : "",
              (flags & TCPFlags::ACK) ? "ACK " : "",
              (flags & TCPFlags::FIN) ? "FIN " : "",
              (flags & TCPFlags::PSH) ? "PSH " : "",
              seq_used, tcb->rcv_nxt, len, tcb->rcv_wnd);

    // 更新序列号
    // 数据占序列号，SYN/FIN 也各占一个序列号
    if (flags & TCPFlags::SYN) {
        tcb->snd_nxt++;
    }
    if (data && len > 0) {
        tcb->snd_nxt += len;
    }
    if (flags & TCPFlags::FIN) {
        tcb->snd_nxt++;
    }

    // 更新活动时间
    tcb->last_activity = std::chrono::steady_clock::now();

    // 发送
    if (_send_cb) {
        _send_cb(tcb->t_tuple.remote_ip, buffer, tcp_len);

        // ─── AI 指标采集: 发包统计 ───
        global_metrics().packets_tx.fetch_add(1, std::memory_order_relaxed);
        global_metrics().bytes_tx.fetch_add(tcp_len, std::memory_order_relaxed);
        tcb->packets_sent_period++;
        if (len > 0) tcb->bytes_sent_period += len;
    }

    // 如果有数据或者是 SYN/FIN，需要加入重传队列
    bool needs_retransmit = (data && len > 0) ||
                            (flags & (TCPFlags::SYN | TCPFlags::FIN));

    if (needs_retransmit) {
        RetransmitEntry entry;
        entry.seq_start = seq_used;
        // seq_end: 数据段用更新后的 snd_nxt，SYN/FIN 在调用处已经更新了 snd_nxt
        entry.seq_end = tcb->snd_nxt;

        if (data && len > 0) {
            std::memcpy(entry.data.data(), data, len);
            entry.data_len = len;
        }

        entry.send_time = std::chrono::steady_clock::now();
        entry.timeout = entry.send_time + std::chrono::microseconds(tcb->rto_us);
        entry.retransmit_count = 0;

        // 先记录 RTT 测量信息（在 move 之前）
        if (!tcb->rtt_measuring) {
            tcb->rtt_measuring = true;
            tcb->rtt_seq = entry.seq_start;
            tcb->rtt_send_time = entry.send_time;
        }

        tcb->retransmit_queue.push_back(std::move(entry));
    }
}

void TCPConnectionManager::raw_send(TCB *tcb, uint8_t flags, uint32_t seq,
                                     const uint8_t *data, size_t len) {
    // 直接发送 TCP 段，不更新 snd_nxt，不加入重传队列
    // 用于重传和特殊场景
    constexpr size_t MAX_TCP_SIZE = 1500 - 20 - 20;
    uint8_t buffer[MAX_TCP_SIZE + sizeof(TCPHeader)];

    // 验证数据长度
    if (len > MAX_TCP_SIZE) {
        LOG_ERROR(TCP, "raw_send: data too large: %zu > %zu", len, MAX_TCP_SIZE);
        return;
    }

    TCPBuilder builder;
    builder.set_src_port(tcb->t_tuple.local_port)
           .set_dst_port(tcb->t_tuple.remote_port)
           .set_seq(seq)
           .set_ack(tcb->rcv_nxt)
           .set_flags(flags)
           .set_window(static_cast<uint16_t>(std::min(tcb->rcv_wnd, 65535u)));

    if (data && len > 0) {
        builder.set_payload(data, len);
    }

    ssize_t tcp_len = builder.build(buffer, sizeof(buffer));
    if (tcp_len < 0) {
        LOG_ERROR(TCP, "Failed to build TCP segment (raw_send)");
        return;
    }

    TCPBuilder::fill_checksum(buffer, tcp_len,
                              tcb->t_tuple.local_ip, tcb->t_tuple.remote_ip);

    LOG_DEBUG(TCP, "raw_send: %s:%u -> %s:%u [%s%s%s%s] seq=%u ack=%u len=%zu",
              ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
              ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port,
              (flags & TCPFlags::SYN) ? "SYN " : "",
              (flags & TCPFlags::ACK) ? "ACK " : "",
              (flags & TCPFlags::FIN) ? "FIN " : "",
              (flags & TCPFlags::PSH) ? "PSH " : "",
              seq, tcb->rcv_nxt, len);

    if (_send_cb) {
        _send_cb(tcb->t_tuple.remote_ip, buffer, tcp_len);
    }
}

void TCPConnectionManager::send_rst(const TCPSegment &seg) {
    LOG_DEBUG(TCP, "Sending RST to %s:%u", ip_to_string(seg.src_addr).c_str(), seg.src_port);

    uint8_t buffer[sizeof(TCPHeader)];

    TCPBuilder builder;
    builder.set_src_port(seg.dst_port)
        .set_dst_port(seg.src_port);

    if (seg.is_ack()) {
        // 对方发了 ACK，RST 的 seq = 对方的 ack
        builder.set_seq(seg.ack_num)
               .set_flags(TCPFlags::RST);
    } else {
        // 对方没发 ACK，RST 的 ack = 对方的 seq + len
        builder.set_seq(0)
               .set_ack(seg.seq_num + seg.seg_len())
               .set_flags(TCPFlags::RST | TCPFlags::ACK);
    }

    ssize_t len = builder.build(buffer, sizeof(buffer));

    if (len > 0) {
        TCPBuilder::fill_checksum(buffer, len, _local_ip, seg.src_addr);
        if (_send_cb) {
            _send_cb(seg.src_addr, buffer, len);
        }
    }
}

void TCPConnectionManager::handle_closed(TCB *tcb, const TCPSegment &seg) {
    if (!seg.is_rst()) {
        send_rst(seg); // 不应该收到任何包，发送 RST
    }
}

void TCPConnectionManager::handle_listen(TCB *listen_tcb, const TCPSegment &seg) {
    // 服务器处理 SYN
    // 只处理纯 SYN，不带 ACK（带 ACK 是同时打开状态，这里不处理）
    if (!seg.is_syn() || seg.is_ack()) {
        return;
    }

    // 连接数上限检查，防止资源耗尽
    if (_connections.size() >= MAX_CONNECTIONS) {
        LOG_WARN(TCP, "Connection limit reached (%zu), dropping SYN from %s:%u",
                 MAX_CONNECTIONS, ip_to_string(seg.src_addr).c_str(), seg.src_port);
        return;
    }

    // 为新连接创建 TCB
    // 监听 TCB 保持不变，继续监听其他连接
    TCPTuple tuple(_local_ip, seg.src_addr, listen_tcb->t_tuple.local_port, seg.src_port);
    TCB *tcb = create_tcb(tuple);
    tcb->passive_open = true;  // 从 LISTEN 创建，被动打开

    // 继承监听 TCB 的回调
    tcb->on_connect = listen_tcb->on_connect;
    tcb->on_receive = listen_tcb->on_receive;
    tcb->on_close = listen_tcb->on_close;

    // 记录对方的初始序列号
    tcb->irs = seg.seq_num;
    tcb->rcv_nxt = seg.seq_num + 1;

    // 生成我们的初始序列号 - 使用四元组确保安全性
    tcb->iss = TCB::generate_isn(tuple.local_ip, tuple.local_port,
                                  tuple.remote_ip, tuple.remote_port);
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;  // send_segment 会在发送 SYN+ACK 时递增

    // 记录对方的窗口大小
    tcb->snd_wnd = seg.window;

    // 发送 SYN+ACK（send_segment 内部 snd_nxt += 1）
    send_segment(tcb, TCPFlags::SYN | TCPFlags::ACK);

    // 进入 SYN_RCVD 状态
    tcb->state = TCPState::SYN_RCVD;

    LOG_INFO(TCP, "LISTEN -> SYN_RCVD: received SYN from %s:%u",
             ip_to_string(seg.src_addr).c_str(), seg.src_port);
}

void TCPConnectionManager::handle_syn_sent(TCB *tcb, const TCPSegment &seg) {
    // 客户端处理 SYN+ACK
    // 正常情况：收到 SYN+ACK
    if (seg.is_syn() && seg.is_ack()) {
        // 验证 ACK 号：对方确认我们的 SYN，因此 ack = iss + 1
        if (seg.ack_num != tcb->iss + 1) {
            LOG_WARN(TCP, "SYN_SENT: invalid ACK %u, expected %u", seg.ack_num, tcb->iss + 1);
            send_rst(seg);
            return;
        }

        // 记录对方的初始序列号
        tcb->irs = seg.seq_num;
        tcb->rcv_nxt = seg.seq_num + 1;

        // 使用 process_ack 来更新 snd_una 并清理重传队列中的 SYN
        process_ack(tcb, seg.ack_num, seg.window);

        // 发送 ACK 完成握手
        send_segment(tcb, TCPFlags::ACK);

        // 进入 ESTABLISHED 状态
        tcb->state = TCPState::ESTABLISHED;

        // ─── AI 指标采集: 连接建立 ───
        global_metrics().active_connections.fetch_add(1, std::memory_order_relaxed);
        global_metrics().conn_established.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO(TCP, "SYN_SENT -> ESTABLISHED: connection established with %s:%u",
                 ip_to_string(seg.src_addr).c_str(), seg.src_port);

        // 通知应用层：连接建立成功
        if (tcb->on_connect) {
            tcb->on_connect(tcb, 0);
        }
        return;
    }

    // 特殊情况：同时打开（两边同时 connect）
    // 只收到 SYN，没有 ACK
    if (seg.is_syn() && !seg.is_ack()) {
        tcb->irs = seg.seq_num;
        tcb->rcv_nxt = seg.seq_num + 1;

        // 清除原来的 SYN 重传条目
        // 新的 SYN+ACK 会在 send_segment 中加入队列
        tcb->retransmit_queue.clear();

        // 发送 SYN+ACK (使用 send_segment 以便加入重传队列)
        // 注意：snd_nxt 已经是 iss + 1 了，需要先回退
        tcb->snd_nxt = tcb->iss;
        send_segment(tcb, TCPFlags::SYN | TCPFlags::ACK);

        tcb->state = TCPState::SYN_RCVD;
        LOG_INFO(TCP, "SYN_SENT -> SYN_RCVD: simultaneous open");
    }
}

void TCPConnectionManager::handle_syn_rcvd(TCB *tcb, const TCPSegment &seg) {
    // 处理 RST
    // RFC 793: 被动打开 → 删除连接TCB，回到 LISTEN；主动打开 → CLOSED
    if (seg.is_rst()) {
        if (tcb->passive_open) {
            // 被动打开：删除这个连接 TCB，监听 TCB 不受影响
            LOG_INFO(TCP, "SYN_RCVD: RST received, returning to LISTEN");
            delete_tcb(tcb);
        } else {
            // 主动打开（同时打开场景）：通知应用层连接失败
            LOG_INFO(TCP, "SYN_RCVD: RST received, connection refused");
            if (tcb->on_connect) {
                tcb->on_connect(tcb, -1);
            }
            tcb->state = TCPState::CLOSED;
            delete_tcb(tcb);
        }
        return;
    }

    // 必须有 ACK
    if (!seg.is_ack()) {
        return;
    }

    // 验证 ACK 号在有效范围内: snd_una < ack <= snd_nxt
    if (!seq_gt(seg.ack_num, tcb->snd_una) || seq_gt(seg.ack_num, tcb->snd_nxt)) {
        send_rst(seg);
        return;
    }

    // 使用 process_ack 来更新状态并清理重传队列中的 SYN+ACK
    process_ack(tcb, seg.ack_num, seg.window);

    // 连接建立
    tcb->state = TCPState::ESTABLISHED;
    LOG_INFO(TCP, "SYN_RCVD -> ESTABLISHED");

    // ─── AI 指标采集: 连接建立 ───
    global_metrics().active_connections.fetch_add(1, std::memory_order_relaxed);
    global_metrics().conn_established.fetch_add(1, std::memory_order_relaxed);

    // 通知应用层
    if (tcb->on_connect) {
        tcb->on_connect(tcb, 0);
    }

    // 如果 ACK 还携带了数据，继续处理
    if (seg.data_length > 0) {
        handle_established(tcb, seg);
    }
}

void TCPConnectionManager::handle_established(TCB *tcb, const TCPSegment &seg) {
    // 处理 ACK（确认发送的数据）
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);
    }

    // 处理数据（使用完整的乱序处理逻辑）
    // 注意：handle_data 内部会发送 ACK
    if (seg.data_length > 0) {
        handle_data(tcb, seg);
    }

    // 处理 FIN（被动关闭）
    // 注意：只有当 FIN 的序列号正好是期望的 rcv_nxt 时才处理
    // 如果 FIN 前面还有乱序数据，需要等数据到齐后再处理 FIN
    if (seg.is_fin()) {
        // FIN 的序列号应该是 seg.seq_num + seg.data_length
        uint32_t fin_seq = seg.seq_num + seg.data_length;

        if (fin_seq == tcb->rcv_nxt) {
            // FIN 正好是期望的，确认它
            tcb->rcv_nxt++;  // FIN 占一个序列号
            send_segment(tcb, TCPFlags::ACK);

            tcb->state = TCPState::CLOSE_WAIT;
            LOG_INFO(TCP, "ESTABLISHED -> CLOSE_WAIT: received FIN");

            if (tcb->on_close) {
                tcb->on_close(tcb);
            }
        } else {
            // FIN 还不能被处理（前面可能有乱序数据）
            // 这种情况应该很少见，暂时记录日志
            LOG_DEBUG(TCP, "FIN received but not at rcv_nxt (fin_seq=%u, rcv_nxt=%u)",
                      fin_seq, tcb->rcv_nxt);
        }
    }
}

void TCPConnectionManager::handle_fin_wait_1(TCB *tcb, const TCPSegment &seg) {
    bool our_fin_acked = false;

    // 处理 ACK（使用 process_ack 来处理重传队列）
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);

        // 检查 FIN 是否被确认
        if (tcb->snd_una == tcb->snd_nxt) {
            our_fin_acked = true;
        }
    }

    // 处理数据（FIN 包可能携带数据）
    if (seg.data_length > 0) {
        handle_data(tcb, seg);
    }

    // 处理 FIN
    if (seg.is_fin()) {
        uint32_t fin_seq = seg.seq_num + seg.data_length;

        if (fin_seq == tcb->rcv_nxt) {
            // FIN 正好是期望的
            tcb->rcv_nxt++;
            send_segment(tcb, TCPFlags::ACK);

            if (our_fin_acked) {
                tcb->state = TCPState::TIME_WAIT;
                start_time_wait_timer(tcb);
                LOG_INFO(TCP, "FIN_WAIT_1 -> TIME_WAIT: both FINs acknowledged");
            } else {
                tcb->state = TCPState::CLOSING;
                LOG_INFO(TCP, "FIN_WAIT_1 -> CLOSING: simultaneous close");
            }
            return;
        } else {
            LOG_DEBUG(TCP, "FIN_WAIT_1: FIN not at rcv_nxt (fin_seq=%u, rcv_nxt=%u)",
                      fin_seq, tcb->rcv_nxt);
        }
    }

    // 只收到 ACK
    if (our_fin_acked) {
        tcb->state = TCPState::FIN_WAIT_2;
        LOG_INFO(TCP, "FIN_WAIT_1 -> FIN_WAIT_2: our FIN acknowledged");
    }
}

void TCPConnectionManager::handle_fin_wait_2(TCB *tcb, const TCPSegment &seg) {
    // 处理 ACK
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);
    }

    // 对方可能还在发数据（使用 handle_data 处理乱序）
    if (seg.data_length > 0) {
        handle_data(tcb, seg);
    }

    // 等待对方的 FIN
    if (seg.is_fin()) {
        uint32_t fin_seq = seg.seq_num + seg.data_length;

        if (fin_seq == tcb->rcv_nxt) {
            // FIN 正好是期望的
            tcb->rcv_nxt++;
            send_segment(tcb, TCPFlags::ACK);
            tcb->state = TCPState::TIME_WAIT;
            start_time_wait_timer(tcb);
            LOG_INFO(TCP, "FIN_WAIT_2 -> TIME_WAIT: received FIN");
        } else {
            LOG_DEBUG(TCP, "FIN_WAIT_2: FIN not at rcv_nxt (fin_seq=%u, rcv_nxt=%u)",
                      fin_seq, tcb->rcv_nxt);
        }
    }
}

void TCPConnectionManager::handle_close_wait(TCB *tcb, const TCPSegment &seg) {
    // 等待应用层调用close()，期间只处理ACK即可
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);
    }
    // 不处理数据和 FIN（因为我们已经收到过 FIN 了）
}

void TCPConnectionManager::handle_closing(TCB *tcb, const TCPSegment &seg) {
    // 等待对方确认我们的 FIN 即可
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);

        // 检查是不是确认了我们的 FIN
        if (tcb->snd_una == tcb->snd_nxt) {
            tcb->state = TCPState::TIME_WAIT;
            start_time_wait_timer(tcb);
            LOG_INFO(TCP, "CLOSING -> TIME_WAIT");
        }
    }
}

void TCPConnectionManager::handle_last_ack(TCB *tcb, const TCPSegment &seg) {
    if (seg.is_ack()) {
        process_ack(tcb, seg.ack_num, seg.window);

        // 检查是不是确认了我们的 FIN
        if (tcb->snd_una == tcb->snd_nxt) {
            LOG_INFO(TCP, "LAST_ACK -> CLOSED: connection closed");
            tcb->state = TCPState::CLOSED;
            delete_tcb(tcb);
        }
    }
}

void TCPConnectionManager::handle_time_wait(TCB *tcb, const TCPSegment &seg) {
    // 如果对方重发 FIN，说明我们的 ACK 丢了
    if (seg.is_fin()) {
        LOG_DEBUG(TCP, "TIME_WAIT: retransmitting ACK for FIN");
        send_segment(tcb, TCPFlags::ACK);
        restart_time_wait_timer(tcb);
    }
}

void TCPConnectionManager::handle_rst(TCB *tcb, const TCPSegment &seg) {
    // 验证 RST 的序列号在接收窗口内（防止伪造的 RST）
    if (!seq_in_range(seg.seq_num, tcb->rcv_nxt, tcb->rcv_nxt + tcb->rcv_wnd)) {
        LOG_DEBUG(TCP, "Ignoring RST with invalid seq %u (expected %u-%u)",
                  seg.seq_num, tcb->rcv_nxt, tcb->rcv_nxt + tcb->rcv_wnd);
        return;
    }

    LOG_INFO(TCP, "%s -> CLOSED: received RST", tcp_state_name(tcb->state));

    // ─── AI 指标采集: RST 导致的连接重置 ───
    global_metrics().conn_reset.fetch_add(1, std::memory_order_relaxed);

    // 在设置 CLOSED 之前减少 active_connections（delete_tcb 不认识 CLOSED 状态）
    if (tcb->state == TCPState::ESTABLISHED ||
        tcb->state == TCPState::FIN_WAIT_1 ||
        tcb->state == TCPState::FIN_WAIT_2 ||
        tcb->state == TCPState::CLOSE_WAIT ||
        tcb->state == TCPState::CLOSING ||
        tcb->state == TCPState::LAST_ACK ||
        tcb->state == TCPState::TIME_WAIT) {
        global_metrics().active_connections.fetch_sub(1, std::memory_order_relaxed);
    }

    // 先设置状态为 CLOSED，防止回调中再发送数据
    tcb->state = TCPState::CLOSED;

    // 通知应用层（注意：回调可能调用 close()，但 close() 在 CLOSED 状态下不做任何事）
    if (tcb->on_close) {
        tcb->on_close(tcb);
    }

    delete_tcb(tcb);
}

void TCPConnectionManager::start_time_wait_timer(TCB *tcb) {
    // TIME_WAIT 条目上限，防止高并发下 TIME_WAIT 积累耗尽内存
    while (_time_wait_list.size() >= MAX_TIME_WAIT_ENTRIES) {
        // 淘汰最旧的条目（列表头部）
        auto &oldest = _time_wait_list.front();
        TCB *old_tcb = find_tcb(oldest.second);
        if (old_tcb && old_tcb->state == TCPState::TIME_WAIT) {
            old_tcb->state = TCPState::CLOSED;
            delete_tcb(old_tcb);
        }
        _time_wait_list.erase(_time_wait_list.begin());
    }

    auto expire_time = std::chrono::steady_clock::now() + TIME_WAIT_DURATION;
    _time_wait_list.emplace_back(expire_time, tcb->t_tuple);

    // TIME_WAIT 只需要保留四元组用于过滤迟到的包，释放缓冲区节省内存
    tcb->send_buffer = StreamBuffer(0);
    tcb->recv_buffer = StreamBuffer(0);
    tcb->retransmit_queue.clear();
    tcb->ooo_queue.clear();
    tcb->ooo_size = 0;

    LOG_DEBUG(TCP, "Started TIME_WAIT timer for %s:%u <-> %s:%u",
              ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
              ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port);
}

void TCPConnectionManager::restart_time_wait_timer(TCB *tcb) {
    // 找到并更新过期时间
    for (auto &entry : _time_wait_list) {
        if (entry.second == tcb->t_tuple) {
            entry.first = std::chrono::steady_clock::now() + TIME_WAIT_DURATION;
            LOG_DEBUG(TCP, "Restarted TIME_WAIT timer");
            return;
        }
    }
    // 如果没找到，就新建一个
    start_time_wait_timer(tcb);
}

void TCPConnectionManager::process_ack(TCB *tcb, uint32_t ack_num, uint16_t window) {
    // 保存旧窗口值，用于检测窗口变化
    uint32_t old_wnd = tcb->snd_wnd;
    tcb->snd_wnd = window;

    // 检查是否是重复 ACK（在更新 snd_una 之前检查）
    check_dup_ack(tcb, ack_num);

    // 如果 ACK 没有确认新数据
    if (!seq_gt(ack_num, tcb->snd_una)) {
        // 即使没有确认新数据，窗口可能变大了（零窗口恢复）
        if (old_wnd == 0 && window > 0) {
            LOG_DEBUG(TCP, "Zero window opened: %u -> %u", old_wnd, window);
            send_buffered_data(tcb);
        }
        return;
    }

    // 计算确认的字节数
    uint32_t bytes_acked = 0;
    if (seq_gt(ack_num, tcb->snd_una)) {
        bytes_acked = ack_num - tcb->snd_una;
        tcb->snd_una = ack_num;
    }

    // 通知拥塞控制
    if (bytes_acked > 0 && tcb->congestion_control) {
        tcb->congestion_control->on_ack(bytes_acked, tcb->srtt_us);
    }

    // ─── AI 指标采集: 确认字节数统计 ───
    tcb->bytes_acked_period += bytes_acked;

    auto now = std::chrono::steady_clock::now();

    // 从队列中移除已经确认的数据
    while (!tcb->retransmit_queue.empty()) {
        auto &entry = tcb->retransmit_queue.front();

        if (seq_le(entry.seq_end, ack_num)) {
            // 如果整个条目已经被确认了

            // RTT测量：只有未重传的段才能用于计算 RTT
            // 使用Karn算法：重传的段无法确定 ACK 对应哪次发送
            if (tcb->rtt_measuring &&
                seq_le(tcb->rtt_seq, entry.seq_end) &&
                entry.retransmit_count == 0) {

                auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(now - tcb->rtt_send_time).count();
                update_rtt(tcb, rtt);
                tcb->rtt_measuring = false;
            }

            tcb->retransmit_queue.pop_front();
        } else if (seq_lt(entry.seq_start, ack_num)) {
            // 部分确认：截断条目
            uint32_t confirmed = ack_num - entry.seq_start;
            if (confirmed <= entry.data.size()) {
                std::memmove(entry.data.data(), entry.data.data() + confirmed, entry.data_len - confirmed);
                entry.data_len -= confirmed;
            } else {
                // SYN/FIN 可能占序列号但没有数据
                entry.data_len = 0;
            }
            entry.seq_start = ack_num;
            break;
        } else {
            // 这个条目还没被确认
            break;
        }
    }

    // 新的 ACK，重置重复 ACK 计数
    tcb->dup_ack_count = 0;
    tcb->last_ack_num = ack_num;

    // 关键：ACK 确认了数据后，窗口空间释放，发送缓冲区中的数据
    if (bytes_acked > 0) {
        send_buffered_data(tcb);
    }

    // ─── AI 指标采集: 定期推送 TCPSample ───
    // 只有在本周期内有发送数据时才采样，避免采集无意义的数据
    // (Discard 服务只收不发，cwnd/bytes_in_flight 永远是初始值)
    if (_metrics_buf && tcb->bytes_sent_period > 0) {
        auto now_tp = std::chrono::steady_clock::now();
        uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now_tp.time_since_epoch()
        ).count();

        if (now_us - tcb->last_sample_time_us >= TCB::SAMPLE_INTERVAL_US) {
            // 计算采样间隔（秒）
            uint64_t interval_us = (tcb->last_sample_time_us > 0)
                                 ? (now_us - tcb->last_sample_time_us)
                                 : TCB::SAMPLE_INTERVAL_US;
            float interval_sec = static_cast<float>(interval_us) / 1000000.0f;

            TCPSample sample{};
            sample.timestamp_us = now_us;
            sample.rtt_us = tcb->last_rtt_us > 0 ? tcb->last_rtt_us : tcb->srtt_us;
            sample.min_rtt_us = tcb->min_rtt_us;
            sample.srtt_us = tcb->srtt_us;
            sample.cwnd = tcb->congestion_control
                        ? tcb->congestion_control->cwnd() / tcb->options.mss
                        : 1;
            sample.ssthresh = tcb->congestion_control
                            ? tcb->congestion_control->ssthresh()
                            : UINT32_MAX;
            sample.bytes_in_flight = (tcb->snd_nxt > tcb->snd_una)
                                   ? tcb->snd_nxt - tcb->snd_una
                                   : 0;

            // 计算交付速率和发送速率 (bytes/s)
            sample.delivery_rate = (interval_sec > 0)
                                 ? static_cast<uint32_t>(tcb->bytes_acked_period / interval_sec)
                                 : 0;
            sample.send_rate = (interval_sec > 0)
                             ? static_cast<uint32_t>(tcb->bytes_sent_period / interval_sec)
                             : 0;

            sample.loss_detected = (tcb->packets_lost_period > 0) ? 1 : 0;
            sample.timeout_occurred = 0;
            sample.ecn_ce_count = tcb->ecn_ce_period;
            sample.is_app_limited = tcb->send_buffer.empty() ? 1 : 0;
            sample.packets_sent = tcb->packets_sent_period;
            sample.packets_lost = tcb->packets_lost_period;

            _metrics_buf->push(sample);

            // 重置周期计数器
            tcb->last_sample_time_us = now_us;
            tcb->packets_sent_period = 0;
            tcb->packets_lost_period = 0;
            tcb->ecn_ce_period = 0;
            tcb->bytes_sent_period = 0;
            tcb->bytes_acked_period = 0;
        }
    }
}

void TCPConnectionManager::update_rtt(TCB *tcb, uint32_t rtt_us) {
    // ─── AI 指标采集: 保存瞬时 RTT ───
    tcb->last_rtt_us = rtt_us;

    // ─── Telemetry: feed RTT histogram ───
    if (auto* hist = telemetry::MetricsRegistry::instance()
            .find_histogram("neustack_tcp_rtt_us")) {
        hist->observe(static_cast<double>(rtt_us));
    }

    // ─── AI 指标采集: 跟踪最小 RTT ───
    // 只在 ESTABLISHED 状态后才更新 min_rtt
    // 原因：TUN 设备的 SYN/SYN-ACK 是本地处理 (~18us)，会污染 min_rtt
    // 真实网络 RTT 通常在几毫秒到几十毫秒
    if (tcb->state == TCPState::ESTABLISHED) {
        if (tcb->min_rtt_us == 0 || rtt_us < tcb->min_rtt_us) {
            tcb->min_rtt_us = rtt_us;
        }
    }

    if (tcb->srtt_us == 0) {
        // 第一次测量
        tcb->srtt_us = rtt_us;
        tcb->rttvar_us = rtt_us / 2;
    } else {
        // 后续测量，采用RFC 6298
        // RTTVAR = (1 - β) * RTTVAR + β * |SRTT - R|, β = 1/4
        uint32_t delta = (tcb->srtt_us > rtt_us) ? (tcb->srtt_us - rtt_us) : (rtt_us - tcb->srtt_us);
        tcb->rttvar_us = (3 * tcb->rttvar_us + delta) / 4;

        // SRTT = (1 - α) * SRTT + α * R, α = 1/8
        tcb->srtt_us = (7 * tcb->srtt_us + rtt_us) / 8;
    }

    // RTO = SRTT + max(G, 4 * RTTVAR)
    // G (时钟粒度) 我们用 100ms = 100000us
    uint32_t rto = tcb->srtt_us + std::max(100000u, 4 * tcb->rttvar_us);

    // RTO限制：最小200ms，最大60s
    tcb->rto_us = std::clamp(rto, 200000u, 60000000u);

    LOG_DEBUG(TCP, "RTT updated: srtt=%u us, rttvar=%u us, rto=%u us",
              tcb->srtt_us, tcb->rttvar_us, tcb->rto_us);
}

void TCPConnectionManager::check_retransmit(TCB *tcb, std::chrono::steady_clock::time_point now) {
    if (tcb->retransmit_queue.empty()) {
        return;
    }

    // 只检查队列头部（最早发送的）
    auto &entry = tcb->retransmit_queue.front();

    if (now < entry.timeout) {
        return; // 还没超时
    }

    // 超时了，需要重传
    entry.retransmit_count++;

    // 通知拥塞控制（超时是严重事件）
    if (tcb->congestion_control) {
        auto *reno = dynamic_cast<TCPReno *>(tcb->congestion_control.get());
        if (reno) {
            reno->on_timeout();
        }
    }

    // 检查是否超过了最大重传次数
    if (entry.retransmit_count > tcb->options.max_retransmit) {
        LOG_WARN(TCP, "Max retransmit reached, closing connection");
        // 通知应用层，关闭连接
        if (tcb->on_close) {
            tcb->on_close(tcb);
        }
        tcb->state = TCPState::CLOSED;
        // 注意：不能在遍历中删除，标记待删除
        return;
    }

    LOG_INFO(TCP, "Retransmit #%d: seq=%u-%u (%u bytes)",
             entry.retransmit_count, entry.seq_start, entry.seq_end,
             entry.length());

    // ─── AI 指标采集: 丢包统计 (超时重传) ───
    tcb->packets_lost_period++;

    // 指数退避：RTO *= 2
    tcb->rto_us = std::min(tcb->rto_us * 2, 60000000u);

    // 更新超时时间
    entry.timeout = now + std::chrono::microseconds(tcb->rto_us);

    // 重传数据
    // 注意：重传不能使用 send_segment（会更新 snd_nxt 和重传队列）
    // 使用 raw_send 直接发送，只发送最多一个 MSS
    if (entry.data_len > 0) { // 数据重传
        size_t retransmit_len = std::min(entry.data_len,
                                          static_cast<size_t>(tcb->options.mss));
        raw_send(tcb, TCPFlags::ACK | TCPFlags::PSH,
                 entry.seq_start, entry.data.data(), retransmit_len);
    } else { // SYN/FIN 重传
        uint8_t flags = 0;
        if (tcb->state == TCPState::SYN_SENT) {
            flags = TCPFlags::SYN;
        } else if (tcb->state == TCPState::SYN_RCVD) {
            flags = TCPFlags::SYN | TCPFlags::ACK;
        } else if (tcb->state == TCPState::FIN_WAIT_1 ||
                   tcb->state == TCPState::LAST_ACK) {
            flags = TCPFlags::FIN | TCPFlags::ACK;
        }
        if (flags) {
            raw_send(tcb, flags, entry.seq_start);
        }
    }

    // 停止 RTT 测量（重传的段不能用于 RTT 计算）
    tcb->rtt_measuring = false;
}

void TCPConnectionManager::check_dup_ack(TCB *tcb, uint32_t ack_num) {
    // 检查是否在快速恢复中
    auto* reno = tcb->congestion_control
                 ? dynamic_cast<TCPReno*>(tcb->congestion_control.get())
                 : nullptr;

    // 只有当 ACK 号等于 snd_una（即没有确认新数据）时才算重复 ACK
    if (ack_num == tcb->snd_una && !tcb->retransmit_queue.empty()) {
        // 如果已经在快速恢复中，只膨胀窗口，不再触发快速重传
        if (reno && reno->in_fast_recovery()) {
            reno->on_dup_ack();
            return;
        }

        tcb->dup_ack_count++;

        // 3个重复ACK触发快速重传并进入快速恢复
        if (tcb->dup_ack_count == 3) {
            LOG_INFO(TCP, "3 dup ACKs, fast retransmit seq=%u", ack_num);

            // ─── AI 指标采集: 丢包统计 (快速重传) ───
            tcb->packets_lost_period++;

            // 通知拥塞控制（进入快速恢复）
            if (tcb->congestion_control) {
                tcb->congestion_control->on_loss(0);
            }

            // 重传第一个未确认的段（使用 raw_send 避免更新状态）
            // 注意：只重传最多一个 MSS 的数据
            auto &entry = tcb->retransmit_queue.front();
            if (entry.data_len > 0) {
                size_t retransmit_len = std::min(entry.data_len,
                                                  static_cast<size_t>(tcb->options.mss));
                raw_send(tcb, TCPFlags::ACK, entry.seq_start,
                         entry.data.data(), retransmit_len);
            }

            // 重置重复 ACK 计数
            tcb->dup_ack_count = 0;
        }
    }
    // 注意：新 ACK 的重置在 process_ack 中处理
}

void TCPConnectionManager::handle_data(TCB *tcb, const TCPSegment &seg) {
    if (seg.data_length == 0) {
        return;
    }

    // 用局部变量，方便调整
    uint32_t seg_start = seg.seq_num;
    uint32_t seg_end = seg.seq_num + seg.data_length;
    const uint8_t *data = seg.data;
    size_t data_len = seg.data_length;

    // 情况1: 完全在期望之前（重复数据）
    if (seq_le(seg_end, tcb->rcv_nxt)) {
        LOG_DEBUG(TCP, "Duplicate data: seq=%u-%u, expected=%u",
                  seg_start, seg_end, tcb->rcv_nxt);
        return;
    }

    // 情况2: 部分重叠（取新的部分）
    if (seq_lt(seg_start, tcb->rcv_nxt)) {
        uint32_t overlap = tcb->rcv_nxt - seg_start;
        if (overlap >= data_len) {
            // 整个段都是重复数据（理论上不会到这里，情况1已处理）
            return;
        }
        seg_start = tcb->rcv_nxt;
        data += overlap;
        data_len -= overlap;
    }

    // 情况3: 正好是期望的数据
    if (seg_start == tcb->rcv_nxt) {
        // 直接交付
        deliver_data(tcb, data, data_len);
        tcb->rcv_nxt = seg_end;

        // 检查乱序队列，看能否交付更多数据
        deliver_ooo_data(tcb);

        // 延迟 ACK 逻辑（只用于顺序到达的数据）
        if (tcb->options.delayed_ack_enabled) {
            if (!tcb->delayed_ack_pending) {
                // 第一个段，启动延迟 ACK 定时器
                tcb->delayed_ack_pending = true;
                tcb->delayed_ack_time = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(tcb->options.delayed_ack_timeout_ms);
            } else {
                // 第二个段，立即发送 ACK（每两个段发一个 ACK）
                send_segment(tcb, TCPFlags::ACK);
                tcb->delayed_ack_pending = false;
            }
        } else {
            // 延迟 ACK 禁用，立即发送 ACK
            send_segment(tcb, TCPFlags::ACK);
        }
        return;
    }

    // 情况4: 乱序数据（seg_start > rcv_nxt）
    LOG_DEBUG(TCP, "Out-of-order: seq=%u-%u, expected=%u",
              seg_start, seg_end, tcb->rcv_nxt);

    // 加入乱序队列
    add_to_ooo_queue(tcb, seg_start, data, data_len);

    // 立即发送重复 ACK（帮助发送方快速重传）
    // 注意：乱序数据不使用延迟 ACK，必须立即响应
    send_segment(tcb, TCPFlags::ACK);
}

void TCPConnectionManager::add_to_ooo_queue(TCB *tcb, uint32_t seq,
                                            const uint8_t *data, size_t len) {
    // 检查缓冲区大小限制
    if (tcb->ooo_size + len > tcb->options.recv_buffer_size) {
        LOG_WARN(TCP, "OOO buffer full, dropping segment");
        return;
    }

    OutOfOrderSegment ooo_seg;
    ooo_seg.seq_start = seq;
    ooo_seg.seq_end = seq + len;
    std::memcpy(ooo_seg.data.data(), data, len);
    ooo_seg.data_len = len;

    // 按序列号顺序插入
    auto it = tcb->ooo_queue.begin();
    while (it != tcb->ooo_queue.end() && seq_lt(it->seq_start, seq)) {
        ++it;
    }

    tcb->ooo_queue.insert(it, std::move(ooo_seg));
    tcb->ooo_size += len;

    LOG_DEBUG(TCP, "Added to OOO queue: seq=%u-%u, queue_size=%zu",
              seq, seq + static_cast<uint32_t>(len), tcb->ooo_size);
}

void TCPConnectionManager::deliver_ooo_data(TCB *tcb) {
    while (!tcb->ooo_queue.empty()) {
        auto &front = tcb->ooo_queue.front();

        // 检查是否可以交付
        if (seq_gt(front.seq_start, tcb->rcv_nxt)) {
            // 还有间隙，无法交付
            break;
        }

        if (seq_le(front.seq_end, tcb->rcv_nxt)) {
            // 完全是重复数据，丢弃
            LOG_DEBUG(TCP, "OOO: discarding duplicate seq=%u-%u",
                      front.seq_start, front.seq_end);
            tcb->ooo_size -= front.data_len;
            tcb->ooo_queue.pop_front();
            continue;
        }

        // 可能有部分重叠
        size_t skip = 0;
        if (seq_lt(front.seq_start, tcb->rcv_nxt)) {
            skip = tcb->rcv_nxt - front.seq_start;
        }

        // 交付数据
        deliver_data(tcb, front.data.data() + skip, front.data_len - skip);
        tcb->rcv_nxt = front.seq_end;

        LOG_DEBUG(TCP, "OOO: delivered seq=%u-%u", front.seq_start, front.seq_end);

        tcb->ooo_size -= front.data_len;
        tcb->ooo_queue.pop_front();
    }
}

void TCPConnectionManager::deliver_data(TCB *tcb, const uint8_t *data, size_t len) {
    LOG_DEBUG(TCP, "Delivered %zu bytes to application", len);

    // 通知应用层
    // 回调模式：数据通过 on_receive 直接交付给应用，无需缓冲
    if (tcb->on_receive) {
        tcb->on_receive(tcb, data, len);
    }

    // 更新接收窗口
    // 对于 echo 类应用，接收窗口应该反映发送缓冲区的剩余空间
    // 这样可以实现背压：当发送缓冲区满时，减小接收窗口，让对端减速
    size_t send_buffer_free = tcb->options.send_buffer_size > tcb->send_buffer.size()
                            ? tcb->options.send_buffer_size - tcb->send_buffer.size()
                            : 0;
    tcb->rcv_wnd = static_cast<uint32_t>(std::min(send_buffer_free,
                                                   static_cast<size_t>(tcb->options.recv_buffer_size)));
}

void TCPConnectionManager::send_buffered_data(TCB *tcb) {
    // 发送缓冲区中的数据
    // 拥塞控制（cwnd）会自动限制发送速度
    while (!tcb->send_buffer.empty()) {
        uint32_t window = tcb->effective_window();
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t available = (window > in_flight) ? (window - in_flight) : 0;

        if (available == 0) {
            break;  // 窗口已满，等待下一个 ACK
        }

        size_t mss = tcb->options.mss;
        auto span = tcb->send_buffer.peek_contiguous();
        size_t to_send = std::min({span.len,
                                   static_cast<size_t>(available), mss});

        send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH,
                     span.data, to_send);

        // 从缓冲区移除已发送的数据
        tcb->send_buffer.consume(to_send);

        LOG_DEBUG(TCP, "Sent %zu bytes from buffer, %zu remaining",
                  to_send, tcb->send_buffer.size());
    }

    // 发送缓冲区释放了空间，更新接收窗口（实现背压释放）
    size_t send_buffer_free = tcb->options.send_buffer_size > tcb->send_buffer.size()
                            ? tcb->options.send_buffer_size - tcb->send_buffer.size()
                            : 0;
    tcb->rcv_wnd = static_cast<uint32_t>(std::min(send_buffer_free,
                                                   static_cast<size_t>(tcb->options.recv_buffer_size)));

    // 缓冲区已空，如果应用层之前调用了 close()，现在完成关闭
    if (tcb->send_buffer.empty() && tcb->close_pending) {
        tcb->close_pending = false;

        if (tcb->state == TCPState::ESTABLISHED) {
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->state = TCPState::FIN_WAIT_1;
            LOG_INFO(TCP, "ESTABLISHED -> FIN_WAIT_1: close pending completed, sending FIN");
        } else if (tcb->state == TCPState::CLOSE_WAIT) {
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->state = TCPState::LAST_ACK;
            LOG_INFO(TCP, "CLOSE_WAIT -> LAST_ACK: close pending completed, sending FIN");
        }
    }
}

void TCPConnectionManager::send_zero_window_probe(TCB *tcb) {
    if (tcb->send_buffer.empty()) {
        return;
    }

    // 取 send_buffer 的第一个字节，但不移除
    uint8_t probe_byte = 0;
    tcb->send_buffer.peek(&probe_byte, 1);

    // 手动构建探测包，不使用 send_segment
    // 因为零窗口探测不应该移动 snd_nxt，也不应该加入重传队列
    uint8_t buffer[sizeof(TCPHeader) + 1];

    TCPBuilder builder;
    builder.set_src_port(tcb->t_tuple.local_port)
           .set_dst_port(tcb->t_tuple.remote_port)
           .set_seq(tcb->snd_nxt)  // 使用当前 snd_nxt，但不移动它
           .set_ack(tcb->rcv_nxt)
           .set_flags(TCPFlags::ACK)
           .set_window(static_cast<uint16_t>(tcb->rcv_wnd))
           .set_payload(&probe_byte, 1);

    ssize_t tcp_len = builder.build(buffer, sizeof(buffer));
    if (tcp_len < 0) {
        return;
    }

    TCPBuilder::fill_checksum(buffer, tcp_len,
                              tcb->t_tuple.local_ip, tcb->t_tuple.remote_ip);

    // 发送，但 **不移动 snd_nxt**，**不加入重传队列**
    if (_send_cb) {
        _send_cb(tcb->t_tuple.remote_ip, buffer, tcp_len);
    }

    LOG_DEBUG(TCP, "Sent zero window probe, seq=%u", tcb->snd_nxt);
}

void TCPConnectionManager::check_zero_window_probe(TCB *tcb, std::chrono::steady_clock::time_point now) {
    // 如果对方窗口为 0 且有数据要发送
    if (tcb->snd_wnd == 0 && !tcb->send_buffer.empty()) {
        if (!tcb->zero_window_probe_needed) {
            // 开始零窗口探测定时器
            tcb->zero_window_probe_needed = true;
            tcb->zwp_time = now + std::chrono::microseconds(tcb->rto_us);
        } else if (now >= tcb->zwp_time) {
            // 发送零窗口探测（使用专门的函数，不移动 snd_nxt）
            send_zero_window_probe(tcb);

            // 重置定时器（指数退避，最大 60 秒）
            uint32_t next_interval = std::min(tcb->rto_us * 2, 60000000u);
            tcb->zwp_time = now + std::chrono::microseconds(next_interval);
        }
    } else {
        tcb->zero_window_probe_needed = false;
    }
}

void TCPConnectionManager::check_delayed_ack(TCB *tcb, std::chrono::steady_clock::time_point now) {
    if (tcb->delayed_ack_pending && now >= tcb->delayed_ack_time) {
        LOG_DEBUG(TCP, "Delayed ACK timeout, sending ACK");
        send_segment(tcb, TCPFlags::ACK);
        tcb->delayed_ack_pending = false;
    }
}

