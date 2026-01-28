#include "neustack/transport/tcp_connection.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

using namespace neustack;

constexpr auto TIME_WAIT_DURATION = std::chrono::seconds(32);

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

int TCPConnectionManager::connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, TCPConnectCallback on_connect) {
    // 构造四元组
    TCPTuple tuple{_local_ip, remote_ip, local_port, remote_port};

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

    // 生成初始序列号（ISN）
    tcb->iss = TCB::generate_isn();
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss + 1;  // SYN 占一个序列号

    // 发送 SYN
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

    // 计算可发送的数据量
    // 受限于：1) 发送窗口 2) 拥塞窗口
    uint32_t window = tcb->effective_window();
    uint32_t in_flight = tcb->snd_nxt - tcb->snd_una; // 已发送未确认的数据量
    uint32_t available = (window > in_flight) ? (window - in_flight) : 0;

    if (available == 0) {
        // 窗口已满，将数据放入发送缓冲区等待
        tcb->send_buffer.insert(tcb->send_buffer.end(), data, data + len);
        LOG_DEBUG(TCP, "Window full, buffered %zu bytes", len);
        return static_cast<ssize_t>(len);
    }

    // 限制单次发送的大小
    constexpr size_t MSS = 1460; // Maximum Segment Size (MTU - IP头 - TCP头)
    size_t to_send = std::min({len, static_cast<size_t>(available), MSS});

    // 发送数据段
    send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH, data, to_send);

    // 剩余数据放入缓冲区
    if (to_send < len) {
        tcb->send_buffer.insert(tcb->send_buffer.end(),
                                data + to_send, data + len);
    }

    LOG_DEBUG(TCP, "Sent %zu bytes, buffered %zu bytes", to_send, len - to_send);
    return static_cast<ssize_t>(len);
}

int TCPConnectionManager::close(TCB *tcb) {
    switch (tcb->state) {
        case TCPState::ESTABLISHED:
            // 发送 FIN，告诉对方我们不再发送数据
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->snd_nxt++; // FIN 占一个序列号
            tcb->state = TCPState::FIN_WAIT_1;
            LOG_INFO(TCP, "ESTABLISHED -> FIN_WAIT_1: initiating close");
            break;

        case TCPState::CLOSE_WAIT:
            // 已经收到了对方的 FIN
            // 应用层处理完后调用 close，现在发送 FIN
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->snd_nxt++;
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

    // TODO: 重传超时处理
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
        LOG_DEBUG(TCP, "Deleted TCB for %s:%u <-> %s:%u",
                  ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
                  ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port);
        _connections.erase(it);
    }
}

void TCPConnectionManager::send_segment(TCB *tcb, uint8_t flags, const uint8_t *data, size_t len) {
    constexpr size_t MAX_TCP_SIZE = 1500 - 20 - 20; // MTU - IP头 - TCP头
    uint8_t buffer[MAX_TCP_SIZE + sizeof(TCPHeader)];

    TCPBuilder builder;
    builder.set_src_port(tcb->t_tuple.local_port)
           .set_dst_port(tcb->t_tuple.remote_port)
           // SYN 时用 snd_una（还没发过数据），否则用 snd_nxt
           .set_seq(flags & TCPFlags::SYN ? tcb->snd_una : tcb->snd_nxt)
           .set_ack(tcb->rcv_nxt)
           .set_flags(flags)
           .set_window(static_cast<uint16_t>(tcb->rcv_wnd));

    if (data && len > 0) {
        builder.set_payload(data, len);
    }

    ssize_t tcp_len = builder.build(buffer, sizeof(buffer));
    if (tcp_len < 0) {
        return;
    }

    // 填充校验和
    TCPBuilder::fill_checksum(buffer, tcp_len,
                              tcb->t_tuple.local_ip, tcb->t_tuple.remote_ip);

    // 更新序列号（数据占序列号，SYN/FIN在调用处单独处理）
    if (data && len > 0) {
        tcb->snd_nxt += len;
    }

    // 更新活动时间
    tcb->last_activity = std::chrono::steady_clock::now();

    // 发送
    if (_send_cb) {
        _send_cb(tcb->t_tuple.remote_ip, buffer, tcp_len);
    }
}

void TCPConnectionManager::send_rst(const TCPSegment &seg) {
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

    // 生成我们的初始序列号
    tcb->iss = TCB::generate_isn();
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss + 1;  // SYN 占一个序列号

    // 记录对方的窗口大小
    tcb->snd_wnd = seg.window;

    // 发送 SYN+ACK
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

        // 更新发送状态
        tcb->snd_una = seg.ack_num;
        tcb->snd_wnd = seg.window;

        // 发送 ACK 完成握手
        send_segment(tcb, TCPFlags::ACK);

        // 进入 ESTABLISHED 状态
        tcb->state = TCPState::ESTABLISHED;

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

    // 更新状态
    tcb->snd_una = seg.ack_num;
    tcb->snd_wnd = seg.window;

    // 连接建立
    tcb->state = TCPState::ESTABLISHED;
    LOG_INFO(TCP, "SYN_RCVD -> ESTABLISHED");

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
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
        }
        tcb->snd_wnd = seg.window;
    }

    // 处理数据
    if (seg.data_length > 0) {
        if (seg.seq_num == tcb->rcv_nxt) {
            // 数据按序到达
            tcb->recv_buffer.insert(tcb->recv_buffer.end(),
                                    seg.data, seg.data + seg.data_length);
            tcb->rcv_nxt += seg.data_length;
            send_segment(tcb, TCPFlags::ACK);

            LOG_DEBUG(TCP, "Received %zu bytes of data", seg.data_length);

            if (tcb->on_receive) {
                tcb->on_receive(tcb, seg.data, seg.data_length);
            }
        } else {
            LOG_DEBUG(TCP, "Out-of-order segment: got seq %u, expected %u",
                      seg.seq_num, tcb->rcv_nxt);
            // TODO: 乱序处理
        }
    }

    // 处理 FIN（被动关闭）
    if (seg.is_fin()) {
        tcb->rcv_nxt = seg.seq_num + seg.data_length + 1;
        send_segment(tcb, TCPFlags::ACK);

        tcb->state = TCPState::CLOSE_WAIT;
        LOG_INFO(TCP, "ESTABLISHED -> CLOSE_WAIT: received FIN");

        if (tcb->on_close) {
            tcb->on_close(tcb);
        }
    }
}

void TCPConnectionManager::handle_fin_wait_1(TCB *tcb, const TCPSegment &seg) {
    bool our_fin_acked = false;

    // 处理 ACK
    if (seg.is_ack()) {
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
            if (tcb->snd_una == tcb->snd_nxt) {
                our_fin_acked = true;
            }
        }
    }

    // 处理 FIN
    if (seg.is_fin()) {
        tcb->rcv_nxt = seg.seq_num + 1;
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
    }

    // 只收到 ACK
    if (our_fin_acked) {
        tcb->state = TCPState::FIN_WAIT_2;
        LOG_INFO(TCP, "FIN_WAIT_1 -> FIN_WAIT_2: our FIN acknowledged");
    }
}

void TCPConnectionManager::handle_fin_wait_2(TCB *tcb, const TCPSegment &seg) {
    // 对方可能还在发数据
    if (seg.data_length > 0 && seg.seq_num == tcb->rcv_nxt) {
        tcb->recv_buffer.insert(tcb->recv_buffer.end(),
                                seg.data, seg.data + seg.data_length);
        tcb->rcv_nxt += seg.data_length;
        send_segment(tcb, TCPFlags::ACK);

        if (tcb->on_receive) {
            tcb->on_receive(tcb, seg.data, seg.data_length);
        }
    }

    // 等待对方的 FIN
    if (seg.is_fin()) {
        tcb->rcv_nxt = seg.seq_num + seg.data_length + 1;
        send_segment(tcb, TCPFlags::ACK);
        tcb->state = TCPState::TIME_WAIT;
        start_time_wait_timer(tcb);
        LOG_INFO(TCP, "FIN_WAIT_2 -> TIME_WAIT: received FIN");
    }
}

void TCPConnectionManager::handle_close_wait(TCB *tcb, const TCPSegment &seg) {
    // 等待应用层调用close()，期间只处理ACK即可
    if (seg.is_ack()) {
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
        }
        tcb->snd_wnd = seg.window;
    }
    // 不处理数据和 FIN（因为我们已经收到过 FIN 了）
}

void TCPConnectionManager::handle_closing(TCB *tcb, const TCPSegment &seg) {
    // 等待对方确认我们的 FIN 即可
    if (seg.is_ack()) {
        // 检查是不是确认的 FIN
        if (seg.ack_num == tcb->snd_nxt) {
            tcb->state = TCPState::TIME_WAIT;
            start_time_wait_timer(tcb);
            LOG_INFO(TCP, "CLOSING -> TIME_WAIT");
        }
    }
}

void TCPConnectionManager::handle_last_ack(TCB *tcb, const TCPSegment &seg) {
    if (seg.is_ack() && seg.ack_num == tcb->snd_nxt) {
        LOG_INFO(TCP, "LAST_ACK -> CLOSED: connection closed");
        tcb->state = TCPState::CLOSED;
        delete_tcb(tcb);
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

    if (tcb->on_close) {
        tcb->on_close(tcb);
    }

    tcb->state = TCPState::CLOSED;
    delete_tcb(tcb);
}

void TCPConnectionManager::start_time_wait_timer(TCB *tcb) {
    auto expire_time = std::chrono::steady_clock::now() + TIME_WAIT_DURATION;
    _time_wait_list.emplace_back(expire_time, tcb->t_tuple);

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
