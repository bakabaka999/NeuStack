# 06: TCP 连接管理 - 状态机实现

上一章我们定义了 TCP 的数据结构。本章实现 TCP 的核心：**连接管理和状态机**。

## 1. 为什么需要连接管理器？

### 1.1 TCP 与 UDP 的本质区别

UDP 是"无连接"的：
- 发送方直接发包，不关心对方是否存在
- 接收方收到包就处理，不需要记住发送方
- 每个包都是独立的，没有上下文

TCP 是"面向连接"的：
- 发送前必须先"建立连接"（三次握手）
- 发送过程中要维护"连接状态"（序列号、窗口、超时等）
- 结束时必须"关闭连接"（四次挥手）

这意味着 **TCP 必须记住每一个连接的状态**。

### 1.2 如何标识一个连接？

一个 TCP 连接由**四元组**唯一标识：

```
(本地 IP, 本地端口, 远程 IP, 远程端口)
```

为什么需要四个值？看这个例子：

```
服务器 192.168.1.1:80 同时处理两个客户端：
  连接 A: (192.168.1.1, 80, 10.0.0.1, 12345)
  连接 B: (192.168.1.1, 80, 10.0.0.2, 54321)
```

只有四元组完全相同，才是同一个连接。

### 1.3 连接管理器的职责

```
┌─────────────────────────────────────────────────────────┐
│                  TCPConnectionManager                    │
├─────────────────────────────────────────────────────────┤
│  职责：                                                  │
│  1. 维护所有活跃连接 (TCB 表)                            │
│  2. 维护所有监听端口 (监听表)                            │
│  3. 收到段时，找到对应的 TCB，按状态机处理                │
│  4. 应用层请求时，创建/关闭连接                          │
│  5. 定时处理超时（重传、TIME_WAIT 等）                   │
├─────────────────────────────────────────────────────────┤
│  数据：                                                  │
│  _connections: { 四元组 -> TCB }  // 活跃连接            │
│  _listeners:   { 端口 -> TCB }    // 监听端口            │
└─────────────────────────────────────────────────────────┘
```

## 2. 为什么要分"监听"和"连接"？

### 2.1 服务器的工作模式

想象一个 Web 服务器监听 80 端口：

```
1. 服务器调用 listen(80)
   - 创建一个"监听 TCB"，状态 = LISTEN
   - 这个 TCB 不对应任何具体连接
   - 它只是说"我愿意接受发往 80 端口的连接请求"

2. 客户端 A 发来 SYN
   - 查表：没有找到四元组匹配的连接
   - 查监听表：找到 80 端口的监听 TCB
   - 创建新的"连接 TCB"，四元组 = (服务器IP, 80, 客户端A_IP, 客户端A端口)
   - 发送 SYN+ACK，新 TCB 状态 = SYN_RCVD

3. 客户端 B 也发来 SYN
   - 同样创建另一个"连接 TCB"
   - 监听 TCB 保持不变，继续等待更多连接
```

关键点：**监听 TCB 是模板，每个新连接都从它复制回调函数**。

### 2.2 两张表的设计

```cpp
// 活跃连接表：四元组 -> TCB
std::unordered_map<TCPTuple, std::unique_ptr<TCB>, TCPTupleHash> _connections;

// 监听表：端口 -> TCB
std::unordered_map<uint16_t, std::unique_ptr<TCB>> _listeners;
```

为什么监听表只用端口做 key？
- 监听时还不知道对方是谁
- 只要本地端口匹配，就用这个监听 TCB 处理

### 2.3 完整的头文件定义

```cpp
// include/neustack/transport/tcp_connection.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP
#define NEUSTACK_TRANSPORT_TCP_CONNECTION_HPP

#include "tcp_tcb.hpp"
#include "tcp_segment.hpp"
#include "tcp_builder.hpp"
#include "tcp_seq.hpp"
#include <unordered_map>
#include <functional>
#include <vector>
#include <chrono>

// ============================================================================
// TCP 发送回调 (由上层提供，用于发送 IP 包)
// ============================================================================

// 参数: dst_ip, tcp_data, tcp_len
using TCPSendCallback = std::function<void(uint32_t, const uint8_t*, size_t)>;

// ============================================================================
// TCP 连接管理器
// ============================================================================

class TCPConnectionManager {
public:
    explicit TCPConnectionManager(uint32_t local_ip);

    // 设置发送回调 (由 IP 层调用)
    void set_send_callback(TCPSendCallback cb) { _send_cb = std::move(cb); }

    // ─── 主动操作 (应用层调用) ───

    // 监听端口 (被动打开)
    // 返回: 0 成功, -1 失败
    int listen(uint16_t port, TCPConnectCallback on_accept);

    // 连接远程主机 (主动打开)
    // 返回: 0 成功 (异步，结果通过回调通知), -1 失败
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                TCPConnectCallback on_connect);

    // 发送数据
    // 返回: 发送的字节数, -1 失败
    ssize_t send(TCB* tcb, const uint8_t* data, size_t len);

    // 关闭连接 (主动关闭)
    int close(TCB* tcb);

    // ─── 被动操作 (收到数据包时调用) ───

    // 处理收到的 TCP 段
    void on_segment_received(const TCPSegment& seg);

    // 定时器触发 (需要定期调用，如每 100ms)
    void on_timer();

private:
    // ─── 内部方法 ───

    // 查找 TCB
    TCB* find_tcb(const TCPTuple& tuple);
    TCB* find_listening_tcb(uint16_t port);

    // 创建新 TCB
    TCB* create_tcb(const TCPTuple& tuple);

    // 删除 TCB
    void delete_tcb(TCB* tcb);

    // 发送 TCP 段
    void send_segment(TCB* tcb, uint8_t flags, const uint8_t* data = nullptr, size_t len = 0);
    void send_rst(const TCPSegment& seg);  // 对无效段发送 RST

    // ─── 状态机处理 ───

    void handle_closed(TCB* tcb, const TCPSegment& seg);
    void handle_listen(TCB* tcb, const TCPSegment& seg);
    void handle_syn_sent(TCB* tcb, const TCPSegment& seg);
    void handle_syn_rcvd(TCB* tcb, const TCPSegment& seg);
    void handle_established(TCB* tcb, const TCPSegment& seg);
    void handle_fin_wait_1(TCB* tcb, const TCPSegment& seg);
    void handle_fin_wait_2(TCB* tcb, const TCPSegment& seg);
    void handle_close_wait(TCB* tcb, const TCPSegment& seg);
    void handle_closing(TCB* tcb, const TCPSegment& seg);
    void handle_last_ack(TCB* tcb, const TCPSegment& seg);
    void handle_time_wait(TCB* tcb, const TCPSegment& seg);

    // RST 处理
    void handle_rst(TCB* tcb, const TCPSegment& seg);

    // 定时器
    void start_time_wait_timer(TCB* tcb);
    void restart_time_wait_timer(TCB* tcb);

    // ─── 数据成员 ───

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
```

## 3. 三次握手：为什么是三次？

### 3.1 握手的目的

三次握手要解决三个问题：

1. **确认双方都能发送和接收**
2. **交换初始序列号 (ISN)**
3. **协商参数（窗口大小等）**

### 3.2 为什么不是两次？

假设只有两次：

```
客户端 ──── SYN ────> 服务器
客户端 <─── SYN+ACK ─ 服务器  (服务器认为连接建立)
```

问题：如果第一个 SYN 在网络中延迟了很久，客户端已经放弃，但服务器收到后会建立连接，浪费资源。

### 3.3 为什么不是四次？

四次可以工作，但三次已经足够：

```
第一次：客户端 -> 服务器，证明客户端能发送
第二次：服务器 -> 客户端，证明服务器能发送和接收
第三次：客户端 -> 服务器，证明客户端能接收
```

第二次的 SYN+ACK 把两个动作合并了（确认对方的 SYN + 发送自己的 SYN）。

### 3.4 握手过程详解

```
    客户端                                      服务器
    (CLOSED)                                  (LISTEN)
       |                                          |
       |  ───────── SYN, seq=100 ─────────────>   |
       |  (我想连接，我的初始序列号是 100)         |
       |                                          |
    (SYN_SENT)                               (SYN_RCVD)
       |                                          |
       |  <──── SYN+ACK, seq=300, ack=101 ─────   |
       |  (好的，我收到了你的 100，期待 101)       |
       |  (我的初始序列号是 300)                   |
       |                                          |
    (ESTABLISHED)                                 |
       |                                          |
       |  ───────── ACK, ack=301 ─────────────>   |
       |  (我收到了你的 300，期待 301)             |
       |                                          |
       |                                    (ESTABLISHED)
```

**关键点：SYN 和 FIN 都占用一个序列号**，所以：
- 收到 seq=100 的 SYN，回复 ack=101（期待下一个是 101）
- 收到 seq=300 的 SYN，回复 ack=301

### 3.5 代码实现

#### 客户端发起连接

```cpp
int TCPConnectionManager::connect(uint32_t remote_ip, uint16_t remote_port,
                                   uint16_t local_port, TCPConnectCallback on_connect) {
    // 1. 构造四元组
    TCPTuple tuple{_local_ip, remote_ip, local_port, remote_port};

    // 2. 检查连接是否已存在
    if (find_tcb(tuple)) {
        return -1;  // 不能重复连接
    }

    // 3. 创建 TCB
    TCB* tcb = create_tcb(tuple);
    tcb->on_connect = std::move(on_connect);

    // 4. 生成初始序列号 (ISN)
    // 为什么要随机？防止旧连接的包被误认为是新连接的
    tcb->iss = TCB::generate_isn();
    tcb->snd_una = tcb->iss;      // 最早未确认的序列号
    tcb->snd_nxt = tcb->iss + 1;  // 下一个要发送的（SYN 占一个序列号）

    // 5. 发送 SYN
    send_segment(tcb, TCPFlags::SYN);

    // 6. 进入 SYN_SENT 状态
    tcb->state = TCPState::SYN_SENT;

    return 0;
}
```

#### 服务器处理 SYN

```cpp
void TCPConnectionManager::handle_listen(TCB* listen_tcb, const TCPSegment& seg) {
    // 只处理纯 SYN（不带 ACK）
    // 为什么检查 ACK？带 ACK 的 SYN 是"同时打开"的情况，这里不处理
    if (!seg.is_syn() || seg.is_ack()) {
        return;
    }

    // 1. 为这个新连接创建 TCB
    // 注意：监听 TCB 保持不变，继续监听其他连接
    TCPTuple tuple{_local_ip, seg.src_addr, listen_tcb->t_tuple.local_port, seg.src_port};
    TCB* tcb = create_tcb(tuple);

    // 2. 继承监听 TCB 的回调
    // 这样应用层只需要在 listen 时设置一次回调
    tcb->on_connect = listen_tcb->on_connect;
    tcb->on_receive = listen_tcb->on_receive;
    tcb->on_close = listen_tcb->on_close;

    // 3. 记录对方的初始序列号
    tcb->irs = seg.seq_num;
    tcb->rcv_nxt = seg.seq_num + 1;  // 期待对方发送 seq_num+1

    // 4. 生成我们的初始序列号
    tcb->iss = TCB::generate_isn();
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss + 1;

    // 5. 记录对方的窗口大小
    // 这决定了我们最多能发多少数据
    tcb->snd_wnd = seg.window;

    // 6. 发送 SYN+ACK
    send_segment(tcb, TCPFlags::SYN | TCPFlags::ACK);

    // 7. 进入 SYN_RCVD 状态
    tcb->state = TCPState::SYN_RCVD;
}
```

#### 客户端处理 SYN+ACK

```cpp
void TCPConnectionManager::handle_syn_sent(TCB* tcb, const TCPSegment& seg) {
    // 正常情况：收到 SYN+ACK
    if (seg.is_syn() && seg.is_ack()) {
        // 验证 ACK 号
        // 对方应该确认我们的 SYN，即 ack = iss + 1
        if (seg.ack_num != tcb->iss + 1) {
            // ACK 号错误，可能是旧连接的包，发送 RST 拒绝
            send_rst(seg);
            return;
        }

        // 记录对方的初始序列号
        tcb->irs = seg.seq_num;
        tcb->rcv_nxt = seg.seq_num + 1;

        // 更新发送状态
        tcb->snd_una = seg.ack_num;  // 我们的 SYN 已被确认
        tcb->snd_wnd = seg.window;

        // 发送 ACK 完成握手
        send_segment(tcb, TCPFlags::ACK);

        // 进入 ESTABLISHED 状态
        tcb->state = TCPState::ESTABLISHED;

        // 通知应用层：连接建立成功
        if (tcb->on_connect) {
            tcb->on_connect(0);  // 0 表示成功
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
    }
}
```

#### 服务器处理 ACK（完成握手）

`SYN_RCVD` 状态需要处理两种情况：
1. 收到 ACK → 完成握手，进入 `ESTABLISHED`
2. 收到 RST → 根据来源返回 `LISTEN`（被动打开）或 `CLOSED`（主动打开/同时打开）

为了区分被动和主动打开，我们需要在 TCB 中添加一个标志：

```cpp
// 在 TCB 结构中添加
bool passive_open = false;  // true = 从 LISTEN 创建，false = 从 connect() 创建
```

完整的处理逻辑：

```cpp
void TCPConnectionManager::handle_syn_rcvd(TCB* tcb, const TCPSegment& seg) {
    // 1. 处理 RST
    // RFC 793: 如果是被动打开，返回 LISTEN；如果是主动打开，返回 CLOSED
    if (seg.is_rst()) {
        if (tcb->passive_open) {
            // 被动打开：删除这个连接 TCB，监听 TCB 继续工作
            LOG_INFO(TCP, "SYN_RCVD: RST received, returning to LISTEN");
            delete_tcb(tcb);
            // 注意：监听 TCB 不受影响，仍在 _listeners 中
        } else {
            // 主动打开（同时打开场景）：通知应用层连接失败
            LOG_INFO(TCP, "SYN_RCVD: RST received, connection refused");
            if (tcb->on_connect) {
                tcb->on_connect(-1);  // -1 表示失败
            }
            tcb->state = TCPState::CLOSED;
            delete_tcb(tcb);
        }
        return;
    }

    // 2. 必须有 ACK
    if (!seg.is_ack()) {
        return;
    }

    // 3. 验证 ACK 号在有效范围内
    // snd_una < ack <= snd_nxt
    if (!seq_gt(seg.ack_num, tcb->snd_una) || seq_gt(seg.ack_num, tcb->snd_nxt)) {
        send_rst(seg);
        return;
    }

    // 4. 更新状态
    tcb->snd_una = seg.ack_num;
    tcb->snd_wnd = seg.window;

    // 5. 连接建立！
    tcb->state = TCPState::ESTABLISHED;
    LOG_INFO(TCP, "Connection established");

    // 6. 通知应用层
    if (tcb->on_connect) {
        tcb->on_connect(0);
    }

    // 7. 如果这个 ACK 还携带了数据，继续处理
    if (seg.data_length > 0) {
        handle_established(tcb, seg);
    }
}
```

同时需要更新 `handle_listen` 和 `handle_syn_sent`，设置 `passive_open` 标志：

```cpp
// 在 handle_listen 中创建新 TCB 时：
TCB* tcb = create_tcb(tuple);
tcb->passive_open = true;  // 被动打开

// 在 handle_syn_sent 进入 SYN_RCVD 时（同时打开）：
tcb->state = TCPState::SYN_RCVD;
// passive_open 保持 false（因为是从 connect() 来的）
```

## 4. 四次挥手：为什么是四次？

### 4.1 为什么不能像握手一样三次？

握手时，SYN+ACK 可以合并，因为：
- 服务器收到 SYN 后，立即就能发 SYN+ACK

挥手时不能合并，因为：
- 收到对方的 FIN 只表示对方不再发送数据
- 但我方可能还有数据要发送
- 必须等我方也准备好了，才能发 FIN

### 4.2 挥手过程详解

```
    主动关闭方                                  被动关闭方
   (ESTABLISHED)                            (ESTABLISHED)
        |                                         |
        |  ─────── FIN, seq=100 ──────────>       |
        |  (我不再发送数据了)                      |
        |                                         |
   (FIN_WAIT_1)                             (CLOSE_WAIT)
        |                                         |
        |  <────── ACK, ack=101 ───────────       |
        |  (好的，我知道了)                        |
        |                                         |
   (FIN_WAIT_2)                                   |
        |                          [被动方可能还在发数据...]
        |                          [应用层调用 close()]
        |                                         |
        |  <────── FIN, seq=300 ───────────       |
        |  (我也不再发送数据了)                    |
        |                                         |
   (TIME_WAIT)                              (LAST_ACK)
        |                                         |
        |  ─────── ACK, ack=301 ──────────>       |
        |  (好的，再见)                            |
        |                                         |
        |  [等待 2MSL...]                    (CLOSED)
        |                                         |
   (CLOSED)
```

### 4.3 为什么需要 TIME_WAIT？

TIME_WAIT 要等待 2MSL（Maximum Segment Lifetime，通常 30-60 秒），原因：

1. **确保最后的 ACK 到达**
   - 如果最后的 ACK 丢失，被动关闭方会重发 FIN
   - TIME_WAIT 状态可以重发 ACK

2. **让旧连接的包消失**
   - 网络中可能还有这个连接的旧包
   - 等 2MSL 后，这些包肯定都过期了
   - 防止旧包干扰使用相同四元组的新连接

### 4.4 代码实现

#### 应用层调用 close

```cpp
int TCPConnectionManager::close(TCB* tcb) {
    switch (tcb->state) {
        case TCPState::ESTABLISHED:
            // 发送 FIN，告诉对方我们不再发送数据
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->snd_nxt++;  // FIN 占一个序列号
            tcb->state = TCPState::FIN_WAIT_1;
            break;

        case TCPState::CLOSE_WAIT:
            // 我们已经收到对方的 FIN
            // 应用层处理完后调用 close，现在发送我们的 FIN
            send_segment(tcb, TCPFlags::FIN | TCPFlags::ACK);
            tcb->snd_nxt++;
            tcb->state = TCPState::LAST_ACK;
            break;

        case TCPState::LISTEN:
        case TCPState::SYN_SENT:
            // 还没建立连接，直接删除
            delete_tcb(tcb);
            break;

        default:
            break;
    }
    return 0;
}
```

#### 收到 FIN（被动关闭）

```cpp
void TCPConnectionManager::handle_established(TCB* tcb, const TCPSegment& seg) {
    // 1. 处理 ACK（确认我们发送的数据）
    if (seg.is_ack()) {
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
        }
        tcb->snd_wnd = seg.window;
    }

    // 2. 处理数据
    if (seg.data_length > 0) {
        if (seg.seq_num == tcb->rcv_nxt) {
            // 数据按序到达
            tcb->recv_buffer.insert(tcb->recv_buffer.end(),
                                    seg.data, seg.data + seg.data_length);
            tcb->rcv_nxt += seg.data_length;
            send_segment(tcb, TCPFlags::ACK);

            if (tcb->on_receive) {
                tcb->on_receive(seg.data, seg.data_length);
            }
        }
        // TODO: 乱序处理
    }

    // 3. 处理 FIN
    if (seg.is_fin()) {
        // FIN 占一个序列号
        tcb->rcv_nxt = seg.seq_num + seg.data_length + 1;
        send_segment(tcb, TCPFlags::ACK);

        // 进入 CLOSE_WAIT 状态
        // 等待应用层调用 close
        tcb->state = TCPState::CLOSE_WAIT;

        // 通知应用层：对方要关闭了
        if (tcb->on_close) {
            tcb->on_close();
        }
    }
}
```

#### FIN_WAIT_1 状态处理

```cpp
void TCPConnectionManager::handle_fin_wait_1(TCB* tcb, const TCPSegment& seg) {
    bool our_fin_acked = false;

    // 处理 ACK
    if (seg.is_ack()) {
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
            // 检查我们的 FIN 是否被确认
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
            // 两边的 FIN 都确认了，直接进入 TIME_WAIT
            tcb->state = TCPState::TIME_WAIT;
            start_time_wait_timer(tcb);
        } else {
            // 同时关闭：对方也发了 FIN，但还没确认我们的 FIN
            tcb->state = TCPState::CLOSING;
        }
        return;
    }

    // 只收到 ACK
    if (our_fin_acked) {
        // 我们的 FIN 被确认，等待对方的 FIN
        tcb->state = TCPState::FIN_WAIT_2;
    }
}
```

#### FIN_WAIT_2 状态处理

```cpp
void TCPConnectionManager::handle_fin_wait_2(TCB* tcb, const TCPSegment& seg) {
    // 对方可能还在发数据
    if (seg.data_length > 0 && seg.seq_num == tcb->rcv_nxt) {
        tcb->recv_buffer.insert(tcb->recv_buffer.end(),
                                seg.data, seg.data + seg.data_length);
        tcb->rcv_nxt += seg.data_length;
        send_segment(tcb, TCPFlags::ACK);

        if (tcb->on_receive) {
            tcb->on_receive(seg.data, seg.data_length);
        }
    }

    // 等待对方的 FIN
    if (seg.is_fin()) {
        tcb->rcv_nxt = seg.seq_num + seg.data_length + 1;
        send_segment(tcb, TCPFlags::ACK);
        tcb->state = TCPState::TIME_WAIT;
        start_time_wait_timer(tcb);
    }
}
```

#### LAST_ACK 状态处理

```cpp
void TCPConnectionManager::handle_last_ack(TCB* tcb, const TCPSegment& seg) {
    if (seg.is_ack() && seg.ack_num == tcb->snd_nxt) {
        // 我们的 FIN 被确认，连接正式关闭
        tcb->state = TCPState::CLOSED;
        delete_tcb(tcb);
    }
}
```

#### TIME_WAIT 状态处理

```cpp
void TCPConnectionManager::handle_time_wait(TCB* tcb, const TCPSegment& seg) {
    // 如果对方重发 FIN（说明我们的 ACK 丢了）
    if (seg.is_fin()) {
        // 重发 ACK
        send_segment(tcb, TCPFlags::ACK);
        // 重置 2MSL 定时器
        restart_time_wait_timer(tcb);
    }
}
```

## 5. RST：紧急刹车

### 5.1 什么时候发送 RST？

1. **收到发给不存在连接的包**（端口没监听）
2. **收到明显错误的包**（ACK 号不对）
3. **应用层异常关闭**（不想正常挥手，直接断开）

### 5.2 RST 的特点

- 不需要对方确认
- 收到 RST 后立即关闭连接
- 不能对 RST 发送 RST（防止死循环）

### 5.3 代码实现

```cpp
void TCPConnectionManager::send_rst(const TCPSegment& seg) {
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

void TCPConnectionManager::handle_rst(TCB* tcb, const TCPSegment& seg) {
    // 验证 RST 的序列号在接收窗口内（防止伪造的 RST）
    if (!seq_in_range(seg.seq_num, tcb->rcv_nxt, tcb->rcv_nxt + tcb->rcv_wnd)) {
        return;  // 忽略
    }

    // 通知应用层
    if (tcb->on_close) {
        tcb->on_close();
    }

    // 立即关闭
    tcb->state = TCPState::CLOSED;
    delete_tcb(tcb);
}
```

## 6. 段处理入口

### 6.1 处理流程

```
收到 TCP 段
    │
    ▼
┌─────────────────┐
│ 1. 查找连接 TCB  │ ─── 找到 ───> 使用该 TCB
└─────────────────┘
    │ 没找到
    ▼
┌─────────────────┐
│ 2. 查找监听 TCB  │ ─── 找到 ───> 使用监听 TCB（可能创建新连接）
└─────────────────┘
    │ 没找到
    ▼
┌─────────────────┐
│ 3. 发送 RST     │ ─── 拒绝这个包
└─────────────────┘
```

### 6.2 代码实现

```cpp
void TCPConnectionManager::on_segment_received(const TCPSegment& seg) {
    // 1. 查找对应的 TCB
    TCPTuple tuple{seg.dst_addr, seg.src_addr, seg.dst_port, seg.src_port};
    TCB* tcb = find_tcb(tuple);

    // 2. 没有已建立的连接，检查监听
    if (!tcb) {
        TCB* listen_tcb = find_listening_tcb(seg.dst_port);
        if (listen_tcb) {
            tcb = listen_tcb;
        } else {
            // 没有监听，发送 RST（但不对 RST 发 RST）
            if (!seg.is_rst()) {
                send_rst(seg);
            }
            return;
        }
    }

    // 3. 先处理 RST
    if (seg.is_rst()) {
        handle_rst(tcb, seg);
        return;
    }

    // 4. 根据状态分发
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
```

## 7. 辅助函数实现

这些函数是状态机正常工作的基础，虽然简单但很重要。

### 7.1 TCB 查找函数

```cpp
// 在活跃连接表中查找 TCB
// 用四元组作为 key
TCB* TCPConnectionManager::find_tcb(const TCPTuple& tuple) {
    auto it = _connections.find(tuple);
    if (it != _connections.end()) {
        return it->second.get();
    }
    return nullptr;
}

// 在监听表中查找 TCB
// 只用本地端口作为 key（因为监听时还不知道对方是谁）
TCB* TCPConnectionManager::find_listening_tcb(uint16_t port) {
    auto it = _listeners.find(port);
    if (it != _listeners.end()) {
        return it->second.get();
    }
    return nullptr;
}
```

### 7.2 TCB 创建和删除

```cpp
// 创建新的连接 TCB 并加入连接表
TCB* TCPConnectionManager::create_tcb(const TCPTuple& tuple) {
    auto tcb = std::make_unique<TCB>();
    tcb->t_tuple = tuple;
    tcb->state = TCPState::CLOSED;
    tcb->last_activity = std::chrono::steady_clock::now();

    TCB* ptr = tcb.get();
    _connections[tuple] = std::move(tcb);

    LOG_DEBUG(TCP, "Created TCB for %s:%u <-> %s:%u",
              ip_to_string(tuple.local_ip).c_str(), tuple.local_port,
              ip_to_string(tuple.remote_ip).c_str(), tuple.remote_port);

    return ptr;
}

// 删除 TCB
// 需要检查是在监听表还是连接表
void TCPConnectionManager::delete_tcb(TCB* tcb) {
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
```

### 7.3 监听函数

```cpp
int TCPConnectionManager::listen(uint16_t port, TCPConnectCallback on_accept) {
    // 检查端口合法性
    if (port == 0) {
        LOG_ERROR(TCP, "Cannot listen on port 0");
        return -1;
    }

    // 检查是否已经在监听
    if (_listeners.count(port)) {
        LOG_WARN(TCP, "Port %u is already being listened on", port);
        return -1;
    }

    // 创建监听 TCB
    // 监听 TCB 的四元组只有 local_ip 和 local_port 有意义
    auto tcb = std::make_unique<TCB>();
    tcb->state = TCPState::LISTEN;
    tcb->t_tuple = TCPTuple{_local_ip, 0, port, 0};  // remote 部分为 0
    tcb->on_connect = std::move(on_accept);

    _listeners[port] = std::move(tcb);

    LOG_INFO(TCP, "Listening on port %u", port);
    return 0;
}
```

### 7.4 TIME_WAIT 定时器

```cpp
// TIME_WAIT 持续时间：2MSL
// MSL (Maximum Segment Lifetime) 通常是 30-60 秒
// 为了测试方便，我们用较短的值
constexpr auto TIME_WAIT_DURATION = std::chrono::seconds(30);

void TCPConnectionManager::start_time_wait_timer(TCB* tcb) {
    auto expire_time = std::chrono::steady_clock::now() + TIME_WAIT_DURATION;
    _time_wait_list.emplace_back(expire_time, tcb->t_tuple);

    LOG_DEBUG(TCP, "Started TIME_WAIT timer for %s:%u <-> %s:%u",
              ip_to_string(tcb->t_tuple.local_ip).c_str(), tcb->t_tuple.local_port,
              ip_to_string(tcb->t_tuple.remote_ip).c_str(), tcb->t_tuple.remote_port);
}

void TCPConnectionManager::restart_time_wait_timer(TCB* tcb) {
    // 找到并更新过期时间
    for (auto& entry : _time_wait_list) {
        if (entry.second == tcb->t_tuple) {
            entry.first = std::chrono::steady_clock::now() + TIME_WAIT_DURATION;
            LOG_DEBUG(TCP, "Restarted TIME_WAIT timer");
            return;
        }
    }
    // 如果没找到，就新建一个
    start_time_wait_timer(tcb);
}
```

### 7.5 剩余的状态处理函数

```cpp
// CLOSED 状态：不应该收到任何包
// 如果收到了，发送 RST
void TCPConnectionManager::handle_closed(TCB* tcb, const TCPSegment& seg) {
    if (!seg.is_rst()) {
        send_rst(seg);
    }
}

// CLOSE_WAIT 状态：我们已经收到对方的 FIN
// 等待应用层调用 close()，在此期间只处理 ACK
void TCPConnectionManager::handle_close_wait(TCB* tcb, const TCPSegment& seg) {
    // 处理 ACK（确认我们之前发送的数据）
    if (seg.is_ack()) {
        if (seq_gt(seg.ack_num, tcb->snd_una) && seq_le(seg.ack_num, tcb->snd_nxt)) {
            tcb->snd_una = seg.ack_num;
        }
        tcb->snd_wnd = seg.window;
    }
    // 不处理数据和 FIN（因为我们已经收到过 FIN 了）
}

// CLOSING 状态：双方同时关闭
// 我们已经发了 FIN，也收到了对方的 FIN，等待对方确认我们的 FIN
void TCPConnectionManager::handle_closing(TCB* tcb, const TCPSegment& seg) {
    if (seg.is_ack()) {
        // 检查是否确认了我们的 FIN
        if (seg.ack_num == tcb->snd_nxt) {
            tcb->state = TCPState::TIME_WAIT;
            start_time_wait_timer(tcb);
            LOG_INFO(TCP, "CLOSING -> TIME_WAIT");
        }
    }
}
```

## 8. 发送段实现

```cpp
void TCPConnectionManager::send_segment(TCB* tcb, uint8_t flags,
                                         const uint8_t* data, size_t len) {
    constexpr size_t MAX_TCP_SIZE = 1500 - 20 - 20;  // MTU - IP头 - TCP头
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

    // 更新序列号（数据占序列号，SYN/FIN 在调用处单独处理）
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
```

## 9. 数据发送实现

应用层通过 `send()` 函数发送数据：

```cpp
ssize_t TCPConnectionManager::send(TCB* tcb, const uint8_t* data, size_t len) {
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
    uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;  // 已发送未确认的数据量
    uint32_t available = (window > in_flight) ? (window - in_flight) : 0;

    if (available == 0) {
        // 窗口已满，将数据放入发送缓冲区等待
        tcb->send_buffer.insert(tcb->send_buffer.end(), data, data + len);
        LOG_DEBUG(TCP, "Window full, buffered %zu bytes", len);
        return static_cast<ssize_t>(len);
    }

    // 限制单次发送的大小
    constexpr size_t MSS = 1460;  // Maximum Segment Size (MTU - IP头 - TCP头)
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
```

## 10. 定时器处理

```cpp
void TCPConnectionManager::on_timer() {
    auto now = std::chrono::steady_clock::now();

    // 处理 TIME_WAIT 超时
    for (auto it = _time_wait_list.begin(); it != _time_wait_list.end(); ) {
        if (now >= it->first) {
            TCB* tcb = find_tcb(it->second);
            if (tcb && tcb->state == TCPState::TIME_WAIT) {
                tcb->state = TCPState::CLOSED;
                delete_tcb(tcb);
            }
            it = _time_wait_list.erase(it);
        } else {
            ++it;
        }
    }

    // TODO: 重传超时处理（下一章）
}
```

## 11. 练习

1. **实现 tcp_connection.hpp 和 tcp_connection.cpp**
   - 按照本章的设计实现所有函数
   - 添加日志便于调试

2. **测试三次握手**
   ```bash
   # 启动你的程序监听 8080 端口
   # 用 nc 测试连接
   nc -v 10.0.0.2 8080
   ```

3. **测试四次挥手**
   - 连接后按 Ctrl+C 断开
   - 观察 FIN/ACK 交换

## 12. 关键点总结

| 概念 | 要点 |
|------|------|
| 四元组 | 唯一标识一个连接 |
| 监听 vs 连接 | 监听是模板，每个连接独立 |
| 三次握手 | 交换 ISN，确认双方能收发 |
| 四次挥手 | 半关闭，每方独立关闭 |
| TIME_WAIT | 等 2MSL，确保旧包消失 |
| RST | 紧急关闭，不需确认 |
| SYN/FIN 占序列号 | 发送后 snd_nxt++ |

## 13. 下一步

本章实现了连接的建立和关闭。下一章我们将实现：

- **可靠传输**：重传机制、超时计算
- **流量控制**：滑动窗口
- **乱序处理**：接收缓冲区重组
