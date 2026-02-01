# 扩展教程 03：高性能环形缓冲区

> **前置要求**: 完成 TCP 基础教程
> **目标**: 理解 Ring Buffer 的设计，优化 TCP 收发缓冲区性能

## 1. 为什么需要 Ring Buffer？

### 1.1 现有实现的问题

我们当前 TCP 的收发缓冲区使用 `std::vector<uint8_t>`：

```cpp
// tcp_tcb.hpp
struct TCB {
    std::vector<uint8_t> send_buffer; // 待发送数据
    std::vector<uint8_t> recv_buffer; // 已接收数据
};
```

这会导致严重的性能问题：

```
场景: 发送数据后收到 ACK，需要删除已确认的头部数据

vector 的操作:
┌───────────────────────────────┐
│ A B C D E F G H I J K L M N   │  发送 ABCD
└───────────────────────────────┘
        ↓ 收到 ACK，删除 ABCD
        ↓ erase(begin, begin+4)
┌───────────────────────────────┐
│ E F G H I J K L M N           │  ← 整体前移！O(n) 拷贝
└───────────────────────────────┘
```

**每次从头部消费数据都触发 O(n) 内存拷贝**。在高吞吐场景（如 1Gbps）下，每秒可能需要确认数万次，这个开销是灾难性的。

### 1.2 Ring Buffer 如何解决

Ring Buffer（环形缓冲区）通过**移动指针而非移动数据**来实现 O(1) 的头部消费：

```
Ring Buffer 结构:

       read_pos          write_pos
          ↓                 ↓
┌───┬───┬───┬───┬───┬───┬───┬───┐
│   │   │ D │ E │ F │ G │   │   │  有效数据: DEFG
└───┴───┴───┴───┴───┴───┴───┴───┘

读取 DE 后: 只移动 read_pos，不拷贝!

                read_pos    write_pos
                   ↓           ↓
┌───┬───┬───┬───┬───┬───┬───┬───┐
│   │   │   │   │ F │ G │   │   │  有效数据: FG
└───┴───┴───┴───┴───┴───┴───┴───┘

写入 HIJ，空间不够时回绕到开头:

  write_pos          read_pos
     ↓                 ↓
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ J │   │   │   │ F │ G │ H │ I │  有效数据: FGHIJ
└───┴───┴───┴───┴───┴───┴───┴───┘
```

### 1.3 性能对比

| 操作 | `vector<uint8_t>` | Ring Buffer |
|------|-------------------|-------------|
| 尾部写入 | O(1) 摊还 | **O(1)** |
| 头部消费 | **O(n) 拷贝** | **O(1) 移指针** |
| 内存分配 | 动态扩容 | **固定大小，零分配** |
| 缓存命中 | 一般 | **连续内存，更友好** |

---

## 2. NeuStack 的两种 Ring Buffer

我们设计了两种针对不同场景优化的 Ring Buffer：

```
┌─────────────────────────────────────────────────────────────┐
│                include/neustack/common/ring_buffer.hpp       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────┐      ┌─────────────────────────┐  │
│  │    StreamBuffer     │      │   MetricsBuffer<T, N>   │  │
│  │    (字节流缓冲区)     │      │   (采样数据缓冲区)       │  │
│  ├─────────────────────┤      ├─────────────────────────┤  │
│  │ • 运行时配置大小     │      │ • 编译期固定大小        │  │
│  │ • 单线程             │      │ • 多线程安全 (SPMC)     │  │
│  │ • 满后拒绝写入       │      │ • 满后覆盖旧数据        │  │
│  │ • 支持连续块读写     │      │ • 无锁原子操作          │  │
│  ├─────────────────────┤      ├─────────────────────────┤  │
│  │ 用于: TCP 收发缓冲区 │      │ 用于: AI 指标采集       │  │
│  └─────────────────────┘      └─────────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. StreamBuffer 详解

### 3.1 头文件

```cpp
#include "neustack/common/ring_buffer.hpp"
```

### 3.2 创建

```cpp
// 默认 64KB
StreamBuffer buf;

// 指定大小
StreamBuffer send_buf(65536);  // 64KB
StreamBuffer recv_buf(131072); // 128KB
```

### 3.3 基本读写

```cpp
StreamBuffer buf(1024);

// 写入数据
uint8_t data[] = "Hello, World!";
size_t written = buf.write(data, sizeof(data));
// written = 14 (包含 '\0')

// 查询状态
buf.size();      // 14 - 当前数据量
buf.capacity();  // 1024 - 总容量
buf.available(); // 1010 - 剩余可写空间
buf.empty();     // false
buf.full();      // false

// 读取数据（拷贝出来，同时移除）
uint8_t out[100];
size_t bytes_read = buf.read(out, sizeof(out));
// bytes_read = 14, buf.size() = 0

// peek: 读取但不移除
buf.write(data, sizeof(data));
size_t peeked = buf.peek(out, 5);  // 只看前 5 字节
// peeked = 5, buf.size() 仍然是 14

// consume: 丢弃数据（不拷贝）
buf.consume(5);  // 丢弃已 peek 的 5 字节
// buf.size() = 9
```

### 3.4 零拷贝操作（关键优化）

对于网络 I/O，我们希望**直接读写缓冲区内存**，避免额外拷贝：

```cpp
// ─── 零拷贝发送 ───

StreamBuffer send_buf(65536);
send_buf.write(app_data, app_len);

// 获取待发送数据的指针（不拷贝）
auto span = send_buf.peek_contiguous();
// span.data = 指向缓冲区内部
// span.len  = 连续可读长度

// 直接发送
ssize_t sent = ::send(sock, span.data, span.len, 0);

// 确认消费
if (sent > 0) {
    send_buf.consume(sent);  // O(1)，只移动指针
}
```

```cpp
// ─── 零拷贝接收 ───

StreamBuffer recv_buf(65536);

// 获取可写入空间的指针
auto span = recv_buf.write_contiguous();
// span.data = 指向可写位置
// span.len  = 连续可写长度

// 直接接收到缓冲区
ssize_t received = ::recv(sock, span.data, span.len, 0);

// 确认写入
if (received > 0) {
    recv_buf.commit_write(received);
}
```

### 3.5 偏移读取（重传场景）

TCP 重传时需要读取已发送但未确认的数据：

```cpp
// 从偏移 100 开始读取 500 字节
uint8_t retrans_data[500];
size_t got = send_buf.peek_at(100, retrans_data, 500);
```

### 3.6 处理回绕

当数据跨越缓冲区末尾时，`peek_contiguous()` 只返回第一段。需要两次读取：

```cpp
// 第一段
auto span1 = buf.peek_contiguous();
process(span1.data, span1.len);
buf.consume(span1.len);

// 如果还有数据，继续读第二段
if (!buf.empty()) {
    auto span2 = buf.peek_contiguous();
    process(span2.data, span2.len);
    buf.consume(span2.len);
}
```

---

## 4. MetricsBuffer 详解

### 4.1 设计目标

MetricsBuffer 用于 **AI 指标采集**：
- TCP 线程高频写入采样数据
- AI 线程定期读取进行推理
- **不能让 AI 阻塞 TCP**

因此使用**无锁设计**：单生产者多消费者 (SPMC)。

### 4.2 创建

```cpp
// 定义采样数据结构
struct TCPSample {
    uint64_t timestamp_us;
    uint32_t rtt_us;
    uint32_t cwnd;
    uint32_t bytes_in_flight;
    float loss_rate;
};

// 创建缓冲区（大小必须是 2 的幂）
MetricsBuffer<TCPSample, 1024> metrics;  // 1024 个样本

// 或使用预定义别名
MetricsBuffer1K<TCPSample> metrics;  // 同上
MetricsBuffer4K<TCPSample> metrics;  // 4096 个样本
```

### 4.3 生产者接口（TCP 线程）

```cpp
// 在 handle_ack 中记录指标
void TCPConnection::on_ack_received(...) {
    // ... 正常处理 ...

    // 记录采样（无锁，O(1)）
    metrics.push({
        .timestamp_us = now_us(),
        .rtt_us = _tcb.metrics.rtt_us,
        .cwnd = _tcb.metrics.cwnd,
        .bytes_in_flight = _tcb.metrics.bytes_in_flight,
        .loss_rate = _tcb.metrics.loss_rate()
    });
}
```

### 4.4 消费者接口（AI 线程）

```cpp
// 获取最新样本
TCPSample latest = metrics.latest();

// 获取最近 10 个样本（从旧到新）
std::vector<TCPSample> recent = metrics.recent(10);

// 用于时序模型
for (const auto& sample : recent) {
    feature_vector.push_back(normalize(sample));
}
```

### 4.5 覆盖语义

MetricsBuffer 满了不会阻塞，而是**覆盖最旧的数据**：

```cpp
MetricsBuffer<int, 4> buf;  // 只能存 4 个

buf.push(1);  // [1, _, _, _]
buf.push(2);  // [1, 2, _, _]
buf.push(3);  // [1, 2, 3, _]
buf.push(4);  // [1, 2, 3, 4]
buf.push(5);  // [5, 2, 3, 4]  ← 覆盖最旧的 1
buf.push(6);  // [5, 6, 3, 4]  ← 覆盖 2

buf.recent(4);  // 返回 [3, 4, 5, 6]（从旧到新）
```

这对于 AI 采集是正确的行为——我们只关心最近的数据。

---

## 5. 集成到 TCP 层

我们的 TCB（`include/neustack/transport/tcp_tcb.hpp`）已经将缓冲区从 `vector<uint8_t>` 改为了 `StreamBuffer`。但 `tcp_connection.cpp` 中的代码还在使用旧的 vector 风格 API，需要逐一迁移。

### 5.1 TCB 的变化（已完成）

```cpp
// include/neustack/transport/tcp_tcb.hpp

#include "neustack/common/ring_buffer.hpp"

struct TCB {
    // ... 其他字段 ...

    // ─── 缓冲区(改用环形缓冲区) ───
    // 旧: std::vector<uint8_t> send_buffer;
    // 旧: std::vector<uint8_t> recv_buffer;
    StreamBuffer send_buffer;
    StreamBuffer recv_buffer;

    // 构造时初始化
    TCB()
        : send_buffer(65536)   // 64KB 发送缓冲区
        , recv_buffer(65536)   // 64KB 接收缓冲区
    {}

    void apply_options(const TCPOptions& opts) {
        send_buffer = StreamBuffer(opts.send_buffer_size);
        recv_buffer = StreamBuffer(opts.recv_buffer_size);
        // ...
    }
};
```

这一步不需要改任何逻辑，只是类型声明。关键在于下面几个函数的 API 迁移。

### 5.2 修改 send() —— 数据写入缓冲区

`TCPConnectionManager::send()` 中多处使用 `vector::insert()` 追加数据，需要改为 `StreamBuffer::write()`。

```cpp
// ─── src/transport/tcp_connection.cpp: send() ───

// 旧代码: 追加到队尾
tcb->send_buffer.insert(tcb->send_buffer.end(), data, data + accepted);

// 新代码: write() 会自动追加，返回实际写入字节数
tcb->send_buffer.write(data, accepted);
```

send() 函数中有 **4 处** `insert()` 调用，分别对应不同的分支（缓冲区已有数据、Nagle 缓冲、窗口满、部分发送后剩余），都需要用 `write()` 替换：

```cpp
// 分支1: 缓冲区已有排队数据，新数据追加到队尾
if (!tcb->send_buffer.empty()) {
    tcb->send_buffer.write(data, accepted);  // 替换 insert()
    return static_cast<ssize_t>(accepted);
}

// 分支2: Nagle 缓冲小包
if (tcb->options.nagle_enabled && ...) {
    tcb->send_buffer.write(data, accepted);  // 替换 insert()
    return static_cast<ssize_t>(accepted);
}

// 分支3: 窗口已满，缓冲等待
if (available == 0) {
    tcb->send_buffer.write(data, accepted);  // 替换 insert()
    return static_cast<ssize_t>(accepted);
}

// 分支4: 部分发送后，剩余数据放入缓冲区
if (to_send < accepted) {
    tcb->send_buffer.write(data + to_send, accepted - to_send);  // 替换 insert()
}
```

缓冲区空间检查也需要适配：

```cpp
// 旧代码: 手动计算 buffer 剩余空间
size_t buffer_space = tcb->options.send_buffer_size > tcb->send_buffer.size()
                    ? tcb->options.send_buffer_size - tcb->send_buffer.size()
                    : 0;

// 新代码: StreamBuffer 直接提供 available()
size_t buffer_space = tcb->send_buffer.available();
```

### 5.3 修改 send_buffered_data() —— 核心性能优化

`TCPConnectionManager::send_buffered_data()` 是**性能优化的核心**。旧代码使用 `vector::data()` 获取指针，然后用 `vector::erase()` 移除已发送数据——这就是 O(n) 的根源。

```cpp
// ─── src/transport/tcp_connection.cpp: send_buffered_data() ───

// 旧代码
void TCPConnectionManager::send_buffered_data(TCB *tcb) {
    while (!tcb->send_buffer.empty()) {
        uint32_t window = tcb->effective_window();
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t available = (window > in_flight) ? (window - in_flight) : 0;
        if (available == 0) break;

        size_t mss = tcb->options.mss;
        size_t to_send = std::min({tcb->send_buffer.size(),
                                   static_cast<size_t>(available), mss});

        // 问题1: data() 对 StreamBuffer 不可用
        send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH,
                     tcb->send_buffer.data(), to_send);

        // 问题2: erase() 是 O(n) 操作!
        tcb->send_buffer.erase(tcb->send_buffer.begin(),
                               tcb->send_buffer.begin() + to_send);
    }
    // ...
}

// 新代码（零拷贝 + O(1) 消费）
void TCPConnectionManager::send_buffered_data(TCB *tcb) {
    while (!tcb->send_buffer.empty()) {
        uint32_t window = tcb->effective_window();
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t available = (window > in_flight) ? (window - in_flight) : 0;
        if (available == 0) break;

        size_t mss = tcb->options.mss;

        // 零拷贝: 直接获取缓冲区内部指针
        auto span = tcb->send_buffer.peek_contiguous();
        size_t to_send = std::min({span.len,
                                   static_cast<size_t>(available), mss});

        send_segment(tcb, TCPFlags::ACK | TCPFlags::PSH,
                     span.data, to_send);  // 直接使用内部指针

        // O(1): 只移动 read_pos，不拷贝内存
        tcb->send_buffer.consume(to_send);
    }
    // ...
}
```

**性能提升**：
- `data()` + `erase()` → `peek_contiguous()` + `consume()`
- 每次发送后的数据移除从 **O(n) 内存拷贝** 变为 **O(1) 指针移动**
- 在高吞吐场景下，这个差异是巨大的

### 5.4 修改零窗口探测

`send_zero_window_probe()` 中使用了 `send_buffer[0]` 获取第一个字节：

```cpp
// 旧代码
uint8_t probe_byte = tcb->send_buffer[0];

// 新代码: 用 peek() 读取第一个字节
uint8_t probe_byte = 0;
tcb->send_buffer.peek(&probe_byte, 1);
```

### 5.5 修改接收窗口计算

多处用 `send_buffer.size()` 计算背压，这部分 **无需修改**，因为 `StreamBuffer` 也有 `size()` 方法，语义一致：

```cpp
// 这些代码不需要改动
size_t send_buffer_free = tcb->options.send_buffer_size > tcb->send_buffer.size()
                        ? tcb->options.send_buffer_size - tcb->send_buffer.size()
                        : 0;

// 但如果愿意，可以更简洁地写为:
size_t send_buffer_free = tcb->send_buffer.available();
```

### 5.6 关于接收缓冲区

我们当前的接收逻辑使用**回调直接交付**——`deliver_data()` 直接把数据通过 `on_receive` 回调传给应用层，不经过 recv_buffer。因此 recv_buffer 目前主要用于背压计算，不是数据的中转。

如果将来需要改为**缓冲模式**（应用层主动 `recv()` 读取），就可以这样使用：

```cpp
// 数据到达时写入缓冲区
void TCPConnectionManager::deliver_data(TCB *tcb, const uint8_t *data, size_t len) {
    size_t written = tcb->recv_buffer.write(data, len);
    if (written < len) {
        // 缓冲区满，更新接收窗口为 0，让对端停止发送
    }
    // 通知应用层有数据可读
}

// 应用层主动读取
ssize_t TCPConnectionManager::recv(TCB *tcb, uint8_t *buf, size_t len) {
    return tcb->recv_buffer.read(buf, len);  // 读取并自动消费
}
```

---

## 6. 关于重传队列

你可能会问：重传队列（`RetransmitEntry`）里也存了 `std::vector<uint8_t> data`，能不能也优化掉？

```cpp
// 当前设计：每个重传条目拷贝一份数据
struct RetransmitEntry {
    uint32_t seq_start;
    uint32_t seq_end;
    std::vector<uint8_t> data;  // 拷贝了一份
    // ...
};
```

理论上可以用 `peek_at()` 从 send_buffer 读取，**但我们不建议这样做**。原因：

1. **ACK 会消费 send_buffer 数据**：收到 ACK 后 `consume()` 移除了已确认数据，但重传条目引用的偏移会随之失效
2. **偏移维护复杂**：每次 ACK 后需要更新所有重传条目的偏移量
3. **收益有限**：重传队列中条目数量通常很少（在途数据有限），拷贝开销不大

**结论**：重传队列保持当前的拷贝设计即可，重点优化 send_buffer 的 `erase()` 性能问题。这是 80/20 法则——用最少的改动获得最大的性能提升。

---

## 7. 完整 API 参考

### 7.1 StreamBuffer

```cpp
class StreamBuffer {
public:
    // 构造
    StreamBuffer();                    // 默认 64KB
    explicit StreamBuffer(size_t cap); // 指定大小

    // 容量查询
    size_t size() const;      // 当前数据量
    size_t capacity() const;  // 总容量
    size_t available() const; // 剩余可写
    bool empty() const;
    bool full() const;

    // 写入
    size_t write(const uint8_t* data, size_t len);

    // 读取
    size_t peek(uint8_t* dest, size_t len) const;  // 读取不移除
    size_t read(uint8_t* dest, size_t len);        // 读取并移除
    size_t consume(size_t len);                     // 只移除不读取
    size_t peek_at(size_t offset, uint8_t* dest, size_t len) const;

    // 零拷贝
    Span peek_contiguous() const;       // 获取可读段
    MutableSpan write_contiguous();     // 获取可写段
    void commit_write(size_t len);      // 确认写入

    // 其他
    void clear();
};
```

### 7.2 MetricsBuffer

```cpp
template <typename T, size_t N>
class MetricsBuffer {
public:
    // 要求: T 必须是 trivially copyable
    // 要求: N 必须是 2 的幂

    MetricsBuffer();

    // 生产者（单线程）
    void push(const T& sample);

    // 消费者（可多线程）
    T latest() const;
    std::vector<T> recent(size_t n) const;
    std::vector<T> all() const;

    // 状态
    size_t count() const;         // 有效样本数
    size_t total_pushed() const;  // 总写入次数
    bool empty() const;
    static constexpr size_t capacity();

    void clear();
};
```

---

## 8. 常见问题

### Q1: 为什么 MetricsBuffer 大小必须是 2 的幂？

为了用位运算代替取模，提高性能：

```cpp
// 慢
index = pos % N;

// 快（当 N 是 2 的幂时）
index = pos & (N - 1);
```

### Q2: StreamBuffer 满了怎么办？

`write()` 会返回实际写入的字节数。如果返回值小于请求长度，说明缓冲区满了：

```cpp
size_t written = buf.write(data, len);
if (written < len) {
    // 处理背压：返回 EAGAIN，或阻塞等待
}
```

### Q3: MetricsBuffer 的线程安全是如何实现的？

使用 `std::atomic` 和内存序：
- 写入：`memory_order_release` 确保数据对读者可见
- 读取：`memory_order_acquire` 确保看到最新数据

这是 **无锁** 实现，不会阻塞任何线程。

### Q4: 能否在 StreamBuffer 上实现多线程？

目前不支持。如果需要多线程访问 TCP 缓冲区，建议在外层加锁，或重新设计为 SPSC（单生产者单消费者）队列。

---

## 9. 下一步

- **Phase 2 教程**: 使用 MetricsBuffer 实现 AI 指标采集
- **性能测试**: Benchmark 对比优化前后的吞吐量

## 10. 参考资料

- [Circular buffer - Wikipedia](https://en.wikipedia.org/wiki/Circular_buffer)
- [Lock-free programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
- [C++ memory order](https://en.cppreference.com/w/cpp/atomic/memory_order)
