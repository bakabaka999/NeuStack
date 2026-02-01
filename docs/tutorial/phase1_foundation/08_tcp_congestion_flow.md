# 08: TCP 拥塞控制与流量控制

上一章我们实现了可靠传输的基础（重传和乱序处理）。本章实现 TCP 的两个重要机制：

1. **拥塞控制**：避免发送太快导致网络拥塞
2. **流量控制**：避免发送太快导致接收方处理不过来

## 1. 拥塞控制 vs 流量控制

这两个机制经常被混淆，但它们解决的是不同的问题：

| | 拥塞控制 | 流量控制 |
|---|---------|---------|
| **保护对象** | 网络（路由器、链路） | 接收方 |
| **问题** | 网络中间设备过载 | 接收方缓冲区满 |
| **信号** | 丢包、延迟增加 | 接收窗口（rwnd） |
| **控制变量** | 拥塞窗口（cwnd） | 接收窗口（rwnd） |

实际发送时，取两者的最小值：

```cpp
effective_window = min(cwnd, rwnd)
```

我们的代码中已经有这个逻辑（`tcp_tcb.hpp:144-148`）：

```cpp
uint32_t effective_window() const {
    uint32_t cwnd = congestion_control ? congestion_control->cwnd() : 65535;
    return std::min(cwnd, snd_wnd);  // snd_wnd 就是对方的 rwnd
}
```

## 2. 拥塞控制算法

### 2.1 经典算法：Reno

TCP Reno 是最经典的拥塞控制算法，包含四个阶段：

```
    cwnd
      │
      │         ×丢包
      │        /│
      │       / │
      │      /  │  快速恢复
      │     /   │  ↓
      │    /    └──────×
      │   / 拥塞避免   /
      │  /           /
      │ / 慢启动    /
      │/___________/________________ time
             ssthresh
```

### 2.2 慢启动 (Slow Start)

**目标**：快速探测网络容量

```
初始: cwnd = 1 MSS (或 IW = 10 MSS，RFC 6928)

每收到一个 ACK:
    cwnd += MSS  (指数增长)

直到:
    cwnd >= ssthresh → 进入拥塞避免
    或发生丢包
```

为什么叫"慢启动"？因为相比于一开始就发送大量数据，它是"慢慢"增加的。但实际上是**指数增长**，很快就能达到网络容量。

### 2.3 拥塞避免 (Congestion Avoidance)

**目标**：谨慎探测，避免拥塞

```
每收到一个 ACK:
    cwnd += MSS * MSS / cwnd  (约等于每 RTT 增加 1 MSS)

或者简化为:
    每个 RTT: cwnd += MSS  (线性增长)
```

这就是著名的 **AIMD** (Additive Increase, Multiplicative Decrease)。

### 2.4 快速重传与快速恢复 (Fast Retransmit & Fast Recovery)

**触发条件**：收到 3 个重复 ACK

```
快速重传:
    立即重传丢失的段（不等超时）

快速恢复 (RFC 5681):
    ssthresh = cwnd / 2
    cwnd = ssthresh + 3 * MSS  (3 个 dup ACK 表示 3 个段已到达)

    对于每个额外的 dup ACK:
        cwnd += MSS  (继续发送新数据)

    收到新 ACK 时:
        cwnd = ssthresh  (退出快速恢复)
```

### 2.5 超时处理

超时是最严重的拥塞信号：

```
ssthresh = max(cwnd / 2, 2 * MSS)
cwnd = 1 MSS  (或 IW)
重新进入慢启动
```

## 3. 实现拥塞控制

### 3.1 利用现有接口

我们已经在 `tcp_tcb.hpp` 中定义了 `ICongestionControl` 接口：

```cpp
class ICongestionControl {
public:
    virtual ~ICongestionControl() = default;

    // 收到 ACK 时调用
    virtual void on_ack(uint32_t bytes_acked, uint32_t rtt_us) = 0;

    // 检测到丢包时调用
    virtual void on_loss(uint32_t bytes_lost) = 0;

    // 获取拥塞窗口（字节）
    virtual uint32_t cwnd() const = 0;

    // 获取慢启动阈值
    virtual uint32_t ssthresh() const = 0;
};
```

### 3.2 实现 Reno 算法

创建 `include/neustack/transport/tcp_reno.hpp`：

```cpp
#ifndef NEUSTACK_TRANSPORT_TCP_RENO_HPP
#define NEUSTACK_TRANSPORT_TCP_RENO_HPP

#include "tcp_tcb.hpp"
#include <algorithm>

// TCP Reno 拥塞控制实现
class TCPReno : public ICongestionControl {
public:
    explicit TCPReno(uint32_t mss = 1460)
        : _mss(mss)
        , _cwnd(mss)                    // 初始 1 MSS
        , _ssthresh(65535)              // 初始很大
        , _in_fast_recovery(false)
        , _recover_seq(0) {}

    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        (void)rtt_us;  // Reno 不使用 RTT

        if (_in_fast_recovery) {
            // 快速恢复中收到新 ACK，退出快速恢复
            _cwnd = _ssthresh;
            _in_fast_recovery = false;
            return;
        }

        if (_cwnd < _ssthresh) {
            // 慢启动：每个 ACK 增加 1 MSS（指数增长）
            _cwnd += _mss;
        } else {
            // 拥塞避免：每个 RTT 增加约 1 MSS（线性增长）
            // cwnd += MSS * MSS / cwnd
            _cwnd += _mss * _mss / _cwnd;
        }

        // 上限
        _cwnd = std::min(_cwnd, 65535u * 16);
    }

    void on_loss(uint32_t bytes_lost) override {
        (void)bytes_lost;

        // 进入快速恢复
        _ssthresh = std::max(_cwnd / 2, 2 * _mss);
        _cwnd = _ssthresh + 3 * _mss;
        _in_fast_recovery = true;
    }

    // 超时时调用（比 on_loss 更严重）
    void on_timeout() {
        _ssthresh = std::max(_cwnd / 2, 2 * _mss);
        _cwnd = _mss;  // 重置为 1 MSS
        _in_fast_recovery = false;
    }

    // 快速恢复中收到重复 ACK
    void on_dup_ack() {
        if (_in_fast_recovery) {
            // 每个 dup ACK 增加 1 MSS（膨胀窗口）
            _cwnd += _mss;
        }
    }

    uint32_t cwnd() const override { return _cwnd; }
    uint32_t ssthresh() const override { return _ssthresh; }

    bool in_fast_recovery() const { return _in_fast_recovery; }

private:
    uint32_t _mss;
    uint32_t _cwnd;
    uint32_t _ssthresh;
    bool _in_fast_recovery;
    uint32_t _recover_seq;  // 进入快速恢复时的 snd_nxt
};

#endif // NEUSTACK_TRANSPORT_TCP_RENO_HPP
```

### 3.3 集成到 TCPConnectionManager

在连接建立时创建拥塞控制器：

```cpp
// tcp_connection.cpp

#include "neustack/transport/tcp_reno.hpp"

TCB* TCPConnectionManager::create_tcb(const TCPTuple& tuple) {
    auto tcb = std::make_unique<TCB>();
    tcb->t_tuple = tuple;
    tcb->state = TCPState::CLOSED;
    tcb->last_activity = std::chrono::steady_clock::now();

    // 创建拥塞控制器
    tcb->congestion_control = std::make_unique<TCPReno>();

    TCB* ptr = tcb.get();
    _connections[tuple] = std::move(tcb);
    return ptr;
}
```

### 3.4 在 ACK 处理中调用

```cpp
void TCPConnectionManager::process_ack(TCB* tcb, uint32_t ack_num) {
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

    // ... 处理重传队列等 ...

    // 关键：ACK 确认了数据后，窗口空间释放，发送缓冲区中的数据
    if (bytes_acked > 0) {
        send_buffered_data(tcb);
    }
}
```

### 3.5 在丢包检测中调用

```cpp
void TCPConnectionManager::check_dup_ack(TCB* tcb, uint32_t ack_num) {
    if (ack_num == tcb->last_ack_num && !tcb->retransmit_queue.empty()) {
        tcb->dup_ack_count++;

        // 通知拥塞控制（快速恢复中膨胀窗口）
        if (tcb->congestion_control) {
            auto* reno = dynamic_cast<TCPReno*>(tcb->congestion_control.get());
            if (reno) {
                reno->on_dup_ack();
            }
        }

        if (tcb->dup_ack_count == 3) {
            LOG_INFO(TCP, "3 dup ACKs, fast retransmit");

            // 通知拥塞控制（进入快速恢复）
            if (tcb->congestion_control) {
                tcb->congestion_control->on_loss(0);
            }

            // 快速重传
            // ...
        }
    } else {
        tcb->dup_ack_count = 0;
        tcb->last_ack_num = ack_num;
    }
}
```

### 3.6 在超时重传中调用

```cpp
void TCPConnectionManager::check_retransmit(TCB* tcb,
    std::chrono::steady_clock::time_point now) {

    // ... 检测到超时 ...

    if (now >= entry.timeout) {
        // 通知拥塞控制（超时是严重事件）
        if (tcb->congestion_control) {
            auto* reno = dynamic_cast<TCPReno*>(tcb->congestion_control.get());
            if (reno) {
                reno->on_timeout();
            }
        }

        // ... 执行重传 ...
    }
}
```

## 4. 流量控制

### 4.1 接收窗口通告

每次发送 ACK 时，都会携带当前的接收窗口大小：

```cpp
void TCPConnectionManager::send_segment(TCB* tcb, uint8_t flags,
                                        const uint8_t* data, size_t len) {
    // ...
    builder.set_window(static_cast<uint16_t>(tcb->rcv_wnd));
    // ...
}
```

### 4.2 动态调整接收窗口

接收窗口应该反映可用的缓冲区空间：

```cpp
// 在 TCB 中添加
static constexpr size_t MAX_RECV_BUFFER = 65535;

// 计算当前可用接收窗口
uint32_t TCB::available_recv_window() const {
    size_t used = recv_buffer.size();
    return (used < MAX_RECV_BUFFER) ? (MAX_RECV_BUFFER - used) : 0;
}
```

在接收数据后更新：

```cpp
void TCPConnectionManager::deliver_data(TCB* tcb, const uint8_t* data, size_t len) {
    tcb->recv_buffer.insert(tcb->recv_buffer.end(), data, data + len);

    // 更新接收窗口
    tcb->rcv_wnd = tcb->available_recv_window();

    // 通知应用层
    if (tcb->on_receive) {
        tcb->on_receive(tcb, data, len);
    }
}
```

### 4.3 零窗口探测

当接收方的窗口变为 0 时，发送方需要定期发送探测包：

```cpp
// 在 TCB 中添加
bool zero_window_probe_needed = false;
std::chrono::steady_clock::time_point zwp_time;

// 在 on_timer 中检查
void TCPConnectionManager::check_zero_window_probe(TCB* tcb,
    std::chrono::steady_clock::time_point now) {

    // 如果对方窗口为 0 且有数据要发送
    if (tcb->snd_wnd == 0 && !tcb->send_buffer.empty()) {
        if (!tcb->zero_window_probe_needed) {
            // 开始零窗口探测定时器
            tcb->zero_window_probe_needed = true;
            tcb->zwp_time = now + std::chrono::seconds(tcb->rto_us / 1000000);
        } else if (now >= tcb->zwp_time) {
            // 发送零窗口探测（1 字节数据）
            LOG_DEBUG(TCP, "Sending zero window probe");

            uint8_t probe_byte = tcb->send_buffer[0];
            send_segment(tcb, TCPFlags::ACK, &probe_byte, 1);

            // 重置定时器（指数退避）
            tcb->zwp_time = now + std::chrono::seconds(
                std::min(tcb->rto_us * 2 / 1000000, 60u));
        }
    } else {
        tcb->zero_window_probe_needed = false;
    }
}
```

### 4.4 窗口更新后发送缓冲数据

当收到 ACK 且窗口增大时，尝试发送缓冲区中的数据：

```cpp
void TCPConnectionManager::process_ack(TCB* tcb, uint32_t ack_num, uint16_t window) {
    uint32_t old_wnd = tcb->snd_wnd;
    tcb->snd_wnd = window;

    // ... 其他 ACK 处理 ...

    // 如果窗口从 0 变为非 0，或者窗口增大，尝试发送缓冲数据
    if ((old_wnd == 0 && window > 0) || window > old_wnd) {
        send_buffered_data(tcb);
    }
}

void TCPConnectionManager::send_buffered_data(TCB* tcb) {
    // 循环发送，直到缓冲区空或窗口用完
    while (!tcb->send_buffer.empty()) {
        uint32_t window = tcb->effective_window();
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t available = (window > in_flight) ? (window - in_flight) : 0;

        if (available == 0) {
            break;  // 窗口已满，等待下一个 ACK
        }

        constexpr size_t MSS = 1460;
        size_t to_send = std::min({tcb->send_buffer.size(),
                                   static_cast<size_t>(available), MSS});

        send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH,
                     tcb->send_buffer.data(), to_send);

        // 从缓冲区移除已发送的数据
        tcb->send_buffer.erase(tcb->send_buffer.begin(),
                               tcb->send_buffer.begin() + to_send);

        LOG_DEBUG(TCP, "Sent %zu bytes from buffer, %zu remaining",
                  to_send, tcb->send_buffer.size());
    }
}
```

### 4.5 Silly Window Syndrome 避免

**Silly Window Syndrome (SWS)** 是指发送大量小包，导致网络效率极低的问题。

想象这个场景：
- 发送端有 1 字节数据要发
- 接收端窗口只剩 1 字节
- 发送端发了一个 1 字节的包（但 TCP/IP 头就占 40 字节！）
- 效率 = 1/41 ≈ 2.4%

这就像用大卡车运一个苹果。

#### 发送端：Nagle 算法

**核心思想**：如果有未确认的数据在飞行中，就把小数据攒起来，等 ACK 回来或攒够一个 MSS 再发。

```cpp
// 在 TCB 中添加
bool nagle_enabled = true;

// 在 send() 中
ssize_t TCPConnectionManager::send(TCB* tcb, const uint8_t* data, size_t len) {
    // ... 状态检查 ...

    constexpr size_t MSS = 1460;

    // Nagle 算法：如果有未确认的数据，且当前数据小于 MSS，就缓冲
    if (tcb->nagle_enabled &&
        tcb->snd_nxt != tcb->snd_una &&  // 有未确认数据
        len < MSS) {                      // 当前数据小于 MSS

        // 缓冲数据，等待 ACK 或凑够 MSS
        tcb->send_buffer.insert(tcb->send_buffer.end(), data, data + len);
        LOG_DEBUG(TCP, "Nagle: buffered %zu bytes", len);
        return static_cast<ssize_t>(len);
    }

    // ... 正常发送 ...
}
```

**Nagle 算法的效果**：

```
没有 Nagle（telnet 每按一个键）:
  [H] [e] [l] [l] [o]  → 5 个包，每个 41 字节 = 205 字节

有 Nagle:
  等第一个 'H' 的 ACK 回来时，后面的 'ello' 已经攒好了
  [H] [ello]  → 2 个包 = 82 字节
```

**何时禁用 Nagle？**

对于需要低延迟的应用（如游戏、实时交互），可以设置 `TCP_NODELAY`：

```cpp
tcb->nagle_enabled = false;  // 禁用 Nagle，立即发送
```

#### 接收端：延迟 ACK

**核心思想**：不要每收到一个段就立即发 ACK，而是：
1. 等收到第二个段时，一起 ACK（捎带确认）
2. 或者等 200ms 超时后再发 ACK

```cpp
// 在 TCB 中添加
bool delayed_ack_pending = false;
std::chrono::steady_clock::time_point delayed_ack_time;
constexpr auto DELAYED_ACK_TIMEOUT = std::chrono::milliseconds(200);

// 收到数据时
void TCPConnectionManager::handle_data(TCB* tcb, const TCPSegment& seg) {
    // ... 处理数据 ...

    // 延迟 ACK 逻辑
    if (!tcb->delayed_ack_pending) {
        // 第一个段，启动延迟 ACK 定时器
        tcb->delayed_ack_pending = true;
        tcb->delayed_ack_time = std::chrono::steady_clock::now() + DELAYED_ACK_TIMEOUT;
    } else {
        // 第二个段，立即发送 ACK（每两个段发一个 ACK）
        send_segment(tcb, TCPFlags::ACK);
        tcb->delayed_ack_pending = false;
    }
}

// 在 on_timer 中检查
void TCPConnectionManager::check_delayed_ack(TCB* tcb,
    std::chrono::steady_clock::time_point now) {

    if (tcb->delayed_ack_pending && now >= tcb->delayed_ack_time) {
        send_segment(tcb, TCPFlags::ACK);
        tcb->delayed_ack_pending = false;
    }
}
```

**延迟 ACK 的效果**：

```
没有延迟 ACK:
  收到 seg1 → 发 ACK1
  收到 seg2 → 发 ACK2
  收到 seg3 → 发 ACK3

有延迟 ACK:
  收到 seg1 → 等待...
  收到 seg2 → 发 ACK2（确认到 seg2）
  收到 seg3 → 等待...
  200ms 超时 → 发 ACK3
```

**注意**：延迟 ACK 和 Nagle 算法可能产生冲突！

```
场景：客户端发请求，等响应
1. 客户端发 "GET /" (小于 MSS)
2. 服务端收到，启动延迟 ACK 定时器
3. 服务端应用层处理请求，准备发响应
4. 服务端想发响应，但 Nagle 等 ACK...
5. 客户端等响应...
6. 死锁！直到 200ms 延迟 ACK 超时

解决：对交互式应用禁用 Nagle (TCP_NODELAY)
```

#### 零窗口探测的特殊处理

零窗口探测也是 SWS 相关的问题。当接收方窗口为 0 时，发送方需要定期探测。

**关键细节**：零窗口探测发送的 1 字节**不应该移动 snd_nxt**！

为什么？
- 这只是个探测，不是正式发送
- 如果对方真的没收下这 1 字节，你把指针挪了，下次正式发数据时序号就乱了

```cpp
void TCPConnectionManager::send_zero_window_probe(TCB* tcb) {
    if (tcb->send_buffer.empty()) {
        return;
    }

    // 取 send_buffer 的第一个字节，但不移除
    uint8_t probe_byte = tcb->send_buffer[0];

    // 手动构建探测包，不使用 send_segment
    // 因为 send_segment 会移动 snd_nxt 和加入重传队列
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
    if (tcp_len < 0) return;

    TCPBuilder::fill_checksum(buffer, tcp_len,
                              tcb->t_tuple.local_ip, tcb->t_tuple.remote_ip);

    // 发送，但 **不移动 snd_nxt**，**不加入重传队列**
    if (_send_cb) {
        _send_cb(tcb->t_tuple.remote_ip, buffer, tcp_len);
    }

    LOG_DEBUG(TCP, "Sent zero window probe, seq=%u", tcb->snd_nxt);
}

void TCPConnectionManager::check_zero_window_probe(TCB* tcb,
    std::chrono::steady_clock::time_point now) {

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
```

**接收端对零窗口探测的响应**：

```
场景 1：窗口还是 0
  - 丢弃探测的 1 字节数据
  - 回复 ACK = snd_nxt（没变），window = 0

场景 2：窗口打开了
  - 收下这 1 字节
  - 回复 ACK = snd_nxt + 1，window > 0
  - 发送方收到这个 ACK 后：
    - process_ack 更新 snd_una
    - 检测到窗口从 0 变为非 0
    - 调用 send_buffered_data 发送缓冲数据
```

## 5. TCB 更新汇总

在 `tcp_tcb.hpp` 中添加的字段：

```cpp
struct TCB {
    // ... 现有字段 ...

    // ─── 拥塞控制 ─── (已有)
    std::unique_ptr<ICongestionControl> congestion_control;

    // ─── 流量控制 ───
    static constexpr size_t MAX_RECV_BUFFER = 65535;

    // 计算可用接收窗口
    uint32_t available_recv_window() const {
        size_t used = recv_buffer.size();
        return (used < MAX_RECV_BUFFER) ?
               static_cast<uint32_t>(MAX_RECV_BUFFER - used) : 0;
    }

    // 零窗口探测
    bool zero_window_probe_needed = false;
    std::chrono::steady_clock::time_point zwp_time;

    // ─── Silly Window Syndrome 避免 ───
    bool nagle_enabled = true;

    // 延迟 ACK
    bool delayed_ack_pending = false;
    std::chrono::steady_clock::time_point delayed_ack_time;

    // ─── 快速重传 ───
    uint32_t dup_ack_count = 0;
    uint32_t last_ack_num = 0;
};
```

## 6. tcp_connection.hpp 新增方法

```cpp
class TCPConnectionManager {
    // ... 现有方法 ...

private:
    // 拥塞控制相关
    void process_ack(TCB* tcb, uint32_t ack_num, uint16_t window);
    void check_dup_ack(TCB* tcb, uint32_t ack_num);

    // 流量控制相关
    void send_buffered_data(TCB* tcb);
    void check_zero_window_probe(TCB* tcb, std::chrono::steady_clock::time_point now);
    void check_delayed_ack(TCB* tcb, std::chrono::steady_clock::time_point now);
};
```

## 7. 定时器处理更新

```cpp
void TCPConnectionManager::on_timer() {
    auto now = std::chrono::steady_clock::now();

    for (auto& [tuple, tcb] : _connections) {
        // 检查重传超时
        check_retransmit(tcb.get(), now);

        // 检查零窗口探测
        check_zero_window_probe(tcb.get(), now);

        // 检查延迟 ACK
        check_delayed_ack(tcb.get(), now);
    }

    // TIME_WAIT 处理
    // ... 现有代码 ...
}
```

## 8. 测试

### 8.1 测试拥塞控制

```cpp
// 观察 cwnd 变化
LOG_DEBUG(TCP, "cwnd=%u, ssthresh=%u",
          tcb->congestion_control->cwnd(),
          tcb->congestion_control->ssthresh());
```

### 8.2 测试流量控制

```bash
# 发送大量数据，观察窗口变化
dd if=/dev/zero bs=1M count=10 | nc 192.168.100.2 7
```

### 8.3 使用 Wireshark

```
观察:
- TCP Window Size: 接收窗口变化
- [TCP ZeroWindow]: 窗口变为 0
- [TCP Window Update]: 窗口更新
- Calculated window size: 实际窗口（考虑 scaling）
```

## 9. 关键点总结

| 机制 | 目的 | 实现要点 |
|------|------|----------|
| 慢启动 | 快速探测容量 | cwnd 指数增长 |
| 拥塞避免 | 谨慎增长 | cwnd 线性增长 |
| 快速重传 | 快速检测丢包 | 3 个 dup ACK |
| 快速恢复 | 避免过度降速 | cwnd 减半而非重置 |
| 零窗口探测 | 避免死锁 | 定期发送 1 字节 |
| Nagle | 减少小包 | 缓冲小数据 |
| 延迟 ACK | 减少 ACK 数量 | 每两个段或 200ms |

## 10. 高级话题（可选）

### 10.1 其他拥塞控制算法

我们实现的 `ICongestionControl` 接口支持插入不同的算法：

- **NewReno**：改进的快速恢复
- **CUBIC**：Linux 默认，适合高带宽网络
- **BBR**：Google 设计，基于带宽和 RTT

```cpp
// 创建时选择算法
tcb->congestion_control = std::make_unique<TCPCubic>();
// 或
tcb->congestion_control = std::make_unique<TCPBBR>();
```

### 10.2 Window Scaling (RFC 7323)

16 位窗口字段最大 65535，对高带宽网络不够用。Window Scaling 选项可以扩展到 1GB：

```cpp
// TCP 选项中协商
// Window Scale = 7 时，实际窗口 = window << 7
```

## 11. 下一步

本章完成了拥塞控制和流量控制的实现。下一章将实现：

- **TCPLayer 封装**：将 TCPConnectionManager 封装为 IProtocolHandler
- **完整集成**：注册到 IPv4Layer，形成完整的 TCP/IP 协议栈
- **API 设计**：提供简洁的应用层接口
