# 07: TCP 可靠传输 - 重传与乱序处理

上一章我们实现了 TCP 连接的建立和关闭。但 TCP 最核心的价值是**可靠传输**——即使网络丢包、乱序、重复，应用层也能收到完整、有序的数据。

本章实现两个关键机制：
1. **超时重传**：发送的数据没收到 ACK，就重新发送
2. **乱序处理**：数据包不按顺序到达时，正确重组

## 1. 为什么需要可靠传输？

### 1.1 网络是不可靠的

IP 层只提供"尽力而为"的服务：
- **丢包**：路由器拥塞、链路故障
- **乱序**：不同路径传输，到达顺序不一致
- **重复**：重传导致同一个包被收到多次
- **损坏**：校验和能检测，但需要重传

```
发送: [1] [2] [3] [4] [5]
网络: [1] [X] [3] [5] [4]  ← 2 丢失，4/5 乱序
接收: [1]  ?  [3] [5] [4]  ← 怎么办？
```

### 1.2 TCP 的解决方案

TCP 使用两个核心机制：

1. **序列号 + ACK**：追踪每个字节，确认收到
2. **重传**：没收到确认就重发

```
发送方                           接收方
   |                                |
   |--- seq=1, "Hello" ------------>|
   |                                | 收到 1-5
   |<-- ack=6 ----------------------| 期待第 6 个字节
   |                                |
   |--- seq=6, "World" ----X        | 丢失！
   |                                |
   |     [超时，没收到 ACK]          |
   |                                |
   |--- seq=6, "World" ------------>| 重传成功
   |<-- ack=11 ---------------------|
```

## 2. 超时重传机制

### 2.1 核心问题：等多久再重传？

等太短：网络稍微延迟就重传，浪费带宽
等太长：真丢包了反应慢，影响性能

**解决方案：动态计算 RTO（Retransmission Timeout）**

### 2.2 RTT 测量与 RTO 计算 (RFC 6298)

RTT（Round-Trip Time）是一个包从发送到收到 ACK 的时间。

```cpp
// 第一次测量
SRTT = R                    // 平滑 RTT
RTTVAR = R / 2              // RTT 方差
RTO = SRTT + 4 * RTTVAR     // 重传超时

// 后续测量
RTTVAR = (1 - β) * RTTVAR + β * |SRTT - R|    // β = 1/4
SRTT = (1 - α) * SRTT + α * R                  // α = 1/8
RTO = SRTT + max(G, 4 * RTTVAR)                // G = 时钟粒度
```

**直观理解**：
- SRTT 是"平均往返时间"
- RTTVAR 是"波动程度"
- RTO = 平均时间 + 安全余量（4 倍波动）

### 2.3 重传队列设计

我们需要记录"已发送但未确认"的数据，以便重传：

```cpp
// 重传队列中的一个条目
struct RetransmitEntry {
    uint32_t seq_start;                              // 起始序列号
    uint32_t seq_end;                                // 结束序列号（不含）
    std::vector<uint8_t> data;                       // 数据副本
    std::chrono::steady_clock::time_point send_time; // 发送时间（用于 RTT）
    std::chrono::steady_clock::time_point timeout;   // 超时时间
    int retransmit_count;                            // 重传次数
};
```

**队列操作**：
1. **发送数据时**：加入重传队列
2. **收到 ACK 时**：从队列移除已确认的数据
3. **定时检查**：超时的条目需要重传

### 2.4 实现重传队列

在 `tcp_tcb.hpp` 中添加：

```cpp
// 重传队列条目
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

struct TCB {
    // ... 现有字段 ...

    // 重传队列
    std::list<RetransmitEntry> retransmit_queue;

    // RTO 计算相关（微秒）
    uint32_t srtt_us = 0;        // 平滑 RTT
    uint32_t rttvar_us = 0;      // RTT 方差
    uint32_t rto_us = 1000000;   // RTO，初始 1 秒

    // 是否有未确认的 RTT 测量
    bool rtt_measuring = false;
    uint32_t rtt_seq;            // 用于测量的序列号
    std::chrono::steady_clock::time_point rtt_send_time;
};
```

### 2.5 发送时加入重传队列

```cpp
void TCPConnectionManager::send_segment(TCB* tcb, uint8_t flags,
                                        const uint8_t* data, size_t len) {
    // ... 构建并发送 TCP 段（现有代码）...

    // 如果有数据或是 SYN/FIN，需要加入重传队列
    bool needs_retransmit = (data && len > 0) ||
                            (flags & (TCPFlags::SYN | TCPFlags::FIN));

    if (needs_retransmit) {
        RetransmitEntry entry;
        entry.seq_start = (flags & TCPFlags::SYN) ? tcb->snd_una : tcb->snd_nxt - len;
        entry.seq_end = tcb->snd_nxt;

        if (data && len > 0) {
            entry.data.assign(data, data + len);
        }

        entry.send_time = std::chrono::steady_clock::now();
        entry.timeout = entry.send_time + std::chrono::microseconds(tcb->rto_us);
        entry.retransmit_count = 0;

        tcb->retransmit_queue.push_back(std::move(entry));

        // 开始 RTT 测量（只测量第一个未确认的段）
        if (!tcb->rtt_measuring) {
            tcb->rtt_measuring = true;
            tcb->rtt_seq = entry.seq_start;
            tcb->rtt_send_time = entry.send_time;
        }
    }
}
```

### 2.6 收到 ACK 时处理重传队列

```cpp
void TCPConnectionManager::process_ack(TCB* tcb, uint32_t ack_num) {
    auto now = std::chrono::steady_clock::now();

    // 从队列中移除已确认的数据
    while (!tcb->retransmit_queue.empty()) {
        auto& entry = tcb->retransmit_queue.front();

        if (seq_le(entry.seq_end, ack_num)) {
            // 整个条目已被确认

            // RTT 测量：只有未重传的段才能用于计算 RTT
            // (Karn 算法：重传的段无法确定 ACK 对应哪次发送)
            if (tcb->rtt_measuring &&
                seq_le(tcb->rtt_seq, entry.seq_end) &&
                entry.retransmit_count == 0) {

                auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - tcb->rtt_send_time).count();
                update_rtt(tcb, rtt);
                tcb->rtt_measuring = false;
            }

            tcb->retransmit_queue.pop_front();
        } else if (seq_lt(entry.seq_start, ack_num)) {
            // 部分确认：截断条目
            uint32_t confirmed = ack_num - entry.seq_start;
            entry.data.erase(entry.data.begin(), entry.data.begin() + confirmed);
            entry.seq_start = ack_num;
            break;
        } else {
            // 这个条目还没被确认
            break;
        }
    }

    // 更新 snd_una
    if (seq_gt(ack_num, tcb->snd_una)) {
        tcb->snd_una = ack_num;
    }
}
```

### 2.7 RTT 更新算法

```cpp
void TCPConnectionManager::update_rtt(TCB* tcb, uint32_t rtt_us) {
    if (tcb->srtt_us == 0) {
        // 第一次测量
        tcb->srtt_us = rtt_us;
        tcb->rttvar_us = rtt_us / 2;
    } else {
        // 后续测量 (RFC 6298)
        // RTTVAR = (1 - β) * RTTVAR + β * |SRTT - R|, β = 1/4
        uint32_t delta = (tcb->srtt_us > rtt_us) ?
                         (tcb->srtt_us - rtt_us) : (rtt_us - tcb->srtt_us);
        tcb->rttvar_us = (3 * tcb->rttvar_us + delta) / 4;

        // SRTT = (1 - α) * SRTT + α * R, α = 1/8
        tcb->srtt_us = (7 * tcb->srtt_us + rtt_us) / 8;
    }

    // RTO = SRTT + max(G, 4 * RTTVAR)
    // G (时钟粒度) 我们用 100ms = 100000us
    uint32_t rto = tcb->srtt_us + std::max(100000u, 4 * tcb->rttvar_us);

    // RTO 限制：最小 200ms，最大 60s
    tcb->rto_us = std::clamp(rto, 200000u, 60000000u);

    LOG_DEBUG(TCP, "RTT updated: srtt=%u us, rttvar=%u us, rto=%u us",
              tcb->srtt_us, tcb->rttvar_us, tcb->rto_us);
}
```

### 2.8 定时器检查重传

在 `on_timer()` 中添加重传检查：

```cpp
void TCPConnectionManager::on_timer() {
    auto now = std::chrono::steady_clock::now();

    // 检查所有连接的重传队列
    for (auto& [tuple, tcb] : _connections) {
        check_retransmit(tcb.get(), now);
    }

    // ... 现有的 TIME_WAIT 处理 ...
}

void TCPConnectionManager::check_retransmit(TCB* tcb,
    std::chrono::steady_clock::time_point now) {

    if (tcb->retransmit_queue.empty()) {
        return;
    }

    // 只检查队列头部（最早发送的）
    auto& entry = tcb->retransmit_queue.front();

    if (now < entry.timeout) {
        return;  // 还没超时
    }

    // 超时了，需要重传
    entry.retransmit_count++;

    // 检查是否超过最大重传次数
    constexpr int MAX_RETRANSMIT = 5;
    if (entry.retransmit_count > MAX_RETRANSMIT) {
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

    // 指数退避：RTO *= 2
    tcb->rto_us = std::min(tcb->rto_us * 2, 60000000u);

    // 更新超时时间
    entry.timeout = now + std::chrono::microseconds(tcb->rto_us);

    // 重传数据
    // 注意：SYN/FIN 的重传需要特殊处理（没有 data）
    if (!entry.data.empty()) {
        send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH,
                     entry.data.data(), entry.data.size());
    } else {
        // SYN 或 FIN 重传
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
            send_segment(tcb, flags);
        }
    }

    // 停止 RTT 测量（重传的段不能用于 RTT 计算）
    tcb->rtt_measuring = false;
}
```

## 3. 乱序处理

### 3.1 问题场景

```
发送: seq=1 [A], seq=2 [B], seq=3 [C], seq=4 [D]
接收: seq=1 [A], seq=3 [C], seq=4 [D], seq=2 [B]
               ↑
           期待 seq=2，但收到 seq=3
```

如果直接丢弃乱序的包，发送方需要重传，效率低。

**解决方案：缓存乱序数据，等待缺失的部分**

### 3.2 乱序缓冲区设计

```cpp
// 乱序数据段
struct OutOfOrderSegment {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;
};

struct TCB {
    // ... 现有字段 ...

    // 乱序缓冲区（按序列号排序）
    std::list<OutOfOrderSegment> ooo_queue;

    // 乱序缓冲区最大大小
    static constexpr size_t MAX_OOO_SIZE = 65535;
    size_t ooo_size = 0;  // 当前乱序缓冲区总大小
};
```

### 3.3 接收数据处理

重写 `handle_established` 中的数据处理部分：

```cpp
void TCPConnectionManager::handle_data(TCB* tcb, const TCPSegment& seg) {
    if (seg.data_length == 0) {
        return;
    }

    uint32_t seg_start = seg.seq_num;
    uint32_t seg_end = seg.seq_num + seg.data_length;

    // 情况 1: 完全在期望之前（重复数据）
    if (seq_le(seg_end, tcb->rcv_nxt)) {
        LOG_DEBUG(TCP, "Duplicate data: seq=%u-%u, expected=%u",
                  seg_start, seg_end, tcb->rcv_nxt);
        return;
    }

    // 情况 2: 部分重叠（取新的部分）
    if (seq_lt(seg_start, tcb->rcv_nxt)) {
        uint32_t overlap = tcb->rcv_nxt - seg_start;
        seg_start = tcb->rcv_nxt;
        // 调整 data 指针
        // seg.data += overlap; seg.data_length -= overlap;
        // 注意：这里需要复制数据，因为 seg 是 const
    }

    // 情况 3: 正好是期望的数据
    if (seg_start == tcb->rcv_nxt) {
        // 直接接收
        deliver_data(tcb, seg.data, seg.data_length);
        tcb->rcv_nxt = seg_end;

        // 检查乱序队列，看能否交付更多数据
        deliver_ooo_data(tcb);

        // 发送 ACK
        send_segment(tcb, TCPFlags::ACK);
        return;
    }

    // 情况 4: 乱序数据（seq_start > rcv_nxt）
    LOG_DEBUG(TCP, "Out-of-order: seq=%u-%u, expected=%u",
              seg_start, seg_end, tcb->rcv_nxt);

    // 加入乱序队列
    add_to_ooo_queue(tcb, seg_start, seg.data, seg.data_length);

    // 发送重复 ACK（帮助发送方快速重传）
    send_segment(tcb, TCPFlags::ACK);
}
```

### 3.4 乱序队列操作

```cpp
void TCPConnectionManager::add_to_ooo_queue(TCB* tcb, uint32_t seq,
                                            const uint8_t* data, size_t len) {
    // 检查缓冲区大小限制
    if (tcb->ooo_size + len > TCB::MAX_OOO_SIZE) {
        LOG_WARN(TCP, "OOO buffer full, dropping segment");
        return;
    }

    OutOfOrderSegment seg;
    seg.seq_start = seq;
    seg.seq_end = seq + len;
    seg.data.assign(data, data + len);

    // 按序列号顺序插入
    auto it = tcb->ooo_queue.begin();
    while (it != tcb->ooo_queue.end() && seq_lt(it->seq_start, seq)) {
        ++it;
    }

    // 检查与前后段的重叠，进行合并
    // 简化版本：直接插入，不合并
    tcb->ooo_queue.insert(it, std::move(seg));
    tcb->ooo_size += len;
}

void TCPConnectionManager::deliver_ooo_data(TCB* tcb) {
    while (!tcb->ooo_queue.empty()) {
        auto& front = tcb->ooo_queue.front();

        // 检查是否可以交付
        if (seq_gt(front.seq_start, tcb->rcv_nxt)) {
            // 还有间隙，无法交付
            break;
        }

        if (seq_le(front.seq_end, tcb->rcv_nxt)) {
            // 完全是重复数据，丢弃
            tcb->ooo_size -= front.data.size();
            tcb->ooo_queue.pop_front();
            continue;
        }

        // 可能有部分重叠
        uint32_t skip = 0;
        if (seq_lt(front.seq_start, tcb->rcv_nxt)) {
            skip = tcb->rcv_nxt - front.seq_start;
        }

        // 交付数据
        deliver_data(tcb, front.data.data() + skip, front.data.size() - skip);
        tcb->rcv_nxt = front.seq_end;

        tcb->ooo_size -= front.data.size();
        tcb->ooo_queue.pop_front();
    }
}

void TCPConnectionManager::deliver_data(TCB* tcb, const uint8_t* data, size_t len) {
    // 加入接收缓冲区
    tcb->recv_buffer.insert(tcb->recv_buffer.end(), data, data + len);

    // 通知应用层
    if (tcb->on_receive) {
        tcb->on_receive(tcb, data, len);
    }

    LOG_DEBUG(TCP, "Delivered %zu bytes to application", len);
}
```

### 3.5 重复 ACK 与快速重传

当接收方收到乱序数据时，会发送重复的 ACK（ack_num 不变）。发送方可以利用这个信号快速检测丢包：

```cpp
// 在 TCB 中添加
uint32_t dup_ack_count = 0;
uint32_t last_ack_num = 0;

// 收到 ACK 时检查
void TCPConnectionManager::check_dup_ack(TCB* tcb, uint32_t ack_num) {
    if (ack_num == tcb->last_ack_num && !tcb->retransmit_queue.empty()) {
        tcb->dup_ack_count++;

        // 3 个重复 ACK 触发快速重传
        if (tcb->dup_ack_count == 3) {
            LOG_INFO(TCP, "3 dup ACKs, fast retransmit seq=%u", ack_num);

            // 重传第一个未确认的段
            auto& entry = tcb->retransmit_queue.front();
            if (!entry.data.empty()) {
                send_segment(tcb, TCPFlags::ACK,
                             entry.data.data(), entry.data.size());
            }

            // 重置重复 ACK 计数
            tcb->dup_ack_count = 0;
        }
    } else {
        // 新的 ACK，重置计数
        tcb->dup_ack_count = 0;
        tcb->last_ack_num = ack_num;
    }
}
```

## 4. 完整的数据接收流程

```
收到 TCP 段
    │
    ▼
┌─────────────────────────────────────┐
│ 1. 验证序列号在接收窗口内            │
│    rcv_nxt <= seq < rcv_nxt + rcv_wnd │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ 2. 处理 ACK                          │
│    - 更新 snd_una                    │
│    - 处理重传队列                     │
│    - 检查重复 ACK                     │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ 3. 处理数据                          │
│    seq == rcv_nxt?                   │
│    ├─ Yes: 交付数据，检查 OOO 队列    │
│    └─ No:  加入 OOO 队列              │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ 4. 处理 FIN                          │
│    - 更新 rcv_nxt                    │
│    - 状态转换                         │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ 5. 发送 ACK                          │
│    - ack = rcv_nxt                   │
└─────────────────────────────────────┘
```

## 5. 头文件更新

### 5.1 tcp_tcb.hpp 新增内容

```cpp
#include <list>

// 重传队列条目
struct RetransmitEntry {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point send_time;
    std::chrono::steady_clock::time_point timeout;
    int retransmit_count = 0;

    uint32_t length() const { return seq_end - seq_start; }
};

// 乱序数据段
struct OutOfOrderSegment {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;

    uint32_t length() const { return seq_end - seq_start; }
};

struct TCB {
    // ... 现有字段 ...

    // ─── 重传相关 ───
    std::list<RetransmitEntry> retransmit_queue;

    // RTO 计算（微秒）
    uint32_t srtt_us = 0;
    uint32_t rttvar_us = 0;
    uint32_t rto_us = 1000000;  // 初始 1 秒

    // RTT 测量
    bool rtt_measuring = false;
    uint32_t rtt_seq = 0;
    std::chrono::steady_clock::time_point rtt_send_time;

    // ─── 乱序处理 ───
    std::list<OutOfOrderSegment> ooo_queue;
    static constexpr size_t MAX_OOO_SIZE = 65535;
    size_t ooo_size = 0;

    // ─── 快速重传 ───
    uint32_t dup_ack_count = 0;
    uint32_t last_ack_num = 0;
};
```

### 5.2 tcp_connection.hpp 新增方法

```cpp
class TCPConnectionManager {
    // ... 现有方法 ...

private:
    // 重传相关
    void process_ack(TCB* tcb, uint32_t ack_num);
    void update_rtt(TCB* tcb, uint32_t rtt_us);
    void check_retransmit(TCB* tcb, std::chrono::steady_clock::time_point now);

    // 乱序处理
    void handle_data(TCB* tcb, const TCPSegment& seg);
    void add_to_ooo_queue(TCB* tcb, uint32_t seq, const uint8_t* data, size_t len);
    void deliver_ooo_data(TCB* tcb);
    void deliver_data(TCB* tcb, const uint8_t* data, size_t len);

    // 快速重传
    void check_dup_ack(TCB* tcb, uint32_t ack_num);
};
```

## 6. 测试可靠传输

### 6.1 测试超时重传

```bash
# 使用 tc（Linux）或 pfctl（macOS）模拟丢包
# macOS 上需要使用 Network Link Conditioner（系统偏好设置）

# 或者修改代码，随机丢弃一些包来测试
```

### 6.2 测试乱序处理

可以通过发送多个并发连接，观察是否正确处理乱序数据。

### 6.3 使用 Wireshark 观察

```
过滤器: tcp.port == 7
观察:
- [TCP Retransmission] 标签表示重传
- [TCP Out-Of-Order] 标签表示乱序
- [TCP Dup ACK] 标签表示重复 ACK
```

## 7. 注意事项

### 7.1 Karn 算法

**重传的段不能用于 RTT 测量**。

为什么？假设发送了一个段，超时后重传。当收到 ACK 时，无法确定这个 ACK 是对第一次发送还是重传的响应。如果用这个时间计算 RTT，可能严重高估或低估。

### 7.2 指数退避

每次重传后，RTO 翻倍：

```cpp
tcb->rto_us = std::min(tcb->rto_us * 2, 60000000u);  // 最大 60 秒
```

这是为了避免网络拥塞时大量重传加剧拥塞。

### 7.3 SYN/FIN 重传

SYN 和 FIN 也需要重传（它们占一个序列号），但没有数据。处理方式：

```cpp
// 重传队列中的条目可能 data 为空
// 根据状态判断是 SYN 还是 FIN
```

## 8. 练习

1. **实现重传队列**
   - 修改 `send_segment` 添加到队列
   - 修改 ACK 处理移除已确认数据
   - 实现 `check_retransmit`

2. **实现乱序处理**
   - 添加 `ooo_queue` 到 TCB
   - 实现 `add_to_ooo_queue` 和 `deliver_ooo_data`

3. **测试**
   - 编写测试程序发送大量数据
   - 人为制造丢包，观察重传
   - 观察 RTT 和 RTO 的变化

## 9. 关键点总结

| 机制 | 作用 | 关键参数 |
|------|------|----------|
| 超时重传 | 检测并恢复丢包 | RTO |
| RTT 测量 | 动态调整 RTO | SRTT, RTTVAR |
| 指数退避 | 避免拥塞恶化 | RTO *= 2 |
| Karn 算法 | 避免错误的 RTT | 不测量重传段 |
| 乱序缓冲 | 提高效率 | ooo_queue |
| 快速重传 | 快速检测丢包 | 3 个重复 ACK |

## 10. 下一步

本章实现了可靠传输的基础。下一章将实现：

- **拥塞控制**：慢启动、拥塞避免、快速恢复
- **流量控制完善**：零窗口探测、Nagle 算法

这些机制将使 TCP 能够在各种网络条件下高效、公平地传输数据。


