# 教程 01：双线程架构 —— 数据面与智能面分离

> **前置要求**: 完成 Phase 1 所有教程，理解 Extra 03 (Ring Buffer)
> **目标**: 实现数据面/智能面双线程架构，建立跨线程通信机制

## 1. 为什么需要双线程？

用户态协议栈的数据面必须保持**低延迟、高吞吐**。但 AI 推理（ONNX Runtime）一次调用可能花费 0.1-1ms。如果在数据面的事件循环里直接调 AI，就会阻塞收包：

```
❌ 单线程方案的问题:

  poll()  process()  AI推理(1ms!)  poll()  process()  AI推理(1ms!)
  ├───────┼──────────┼─────────────┼───────┼──────────┼─────────────
  0       50us       100us         1100us  1150us     1200us

  AI 推理期间收到的包只能排在 NIC 队列里等着
  如果每 10ms 推理一次，数据面有 10% 的时间在空转
```

**架构 B 的解决方案**: 把 AI 推理放到独立线程：

```
✅ 双线程方案:

  Thread 0 (数据面):  poll → process → poll → process → poll → ...
                       │                │
                       ▼ push sample    ▼ pop action
                       ·                ·
  Thread 1 (智能面):   ·  sleep(10ms) → read samples → AI推理 → push action
                       ·                                         │
                       ◂────────────────────────────────────────┘

  数据面永不阻塞，AI 推理在另一个核上并行运行
```

## 2. 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    NeuStack 架构 B                                │
│                                                                  │
│  ┌─────────────────────────────────┐                            │
│  │  Thread 0: 数据面               │                            │
│  │  (绑核，busy-poll，永不让出)     │                            │
│  │                                 │                            │
│  │  while(1) {                     │                            │
│  │    pkt = hal.receive();         │                            │
│  │    ip.handle(pkt);              │                            │
│  │    tcp.handle(pkt);             │                            │
│  │                                 │                            │
│  │    // 高频采样 → 无锁写入       │                            │
│  │    metrics_buf.push(sample);  ──┼─── MetricsBuffer ──┐      │
│  │                                 │    (SPMC, 覆盖旧数据) │      │
│  │    // 检查 AI 决策              │                      │      │
│  │    if (action_q.try_pop(act)) ◀─┼─── SPSCQueue ───┐  │      │
│  │      apply(act);                │    (SPSC, 拒绝满写) │  │      │
│  │  }                              │                   │  │      │
│  └─────────────────────────────────┘                   │  │      │
│                                                        │  │      │
│  ┌─────────────────────────────────┐                   │  │      │
│  │  Thread 1: 智能面               │                   │  │      │
│  │  (可被调度，允许 sleep)          │                   │  │      │
│  │                                 │                   │  │      │
│  │  while(running) {               │                   │  │      │
│  │    sleep(10ms);                 │                   │  │      │
│  │                                 │                   │  │      │
│  │    // 读采样数据                │                   │  │      │
│  │    samples = buf.recent(100); ◀─┼───────────────────┘  │      │
│  │    snapshot = global.snapshot();│                      │      │
│  │                                 │                      │      │
│  │    // AI 推理                   │                      │      │
│  │    features = extract(samples); │                      │      │
│  │    action = onnx.infer(features)│                      │      │
│  │                                 │                      │      │
│  │    // 决策传回                  │                      │      │
│  │    action_q.try_push(action); ──┼──────────────────────┘      │
│  │  }                              │                             │
│  └─────────────────────────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 跨线程通信一览

| 通道 | 方向 | 类型 | 满了怎么办 | 已有/新增 |
|------|------|------|-----------|----------|
| `MetricsBuffer<TCPSample, 1024>` | 数据面 → 智能面 | SPMC | 覆盖旧数据 | **已有** |
| `GlobalMetrics` (atomic) | 数据面 → 智能面 | 共享读写 | N/A (计数器) | **新增** |
| `SPSCQueue<AIAction, 16>` | 智能面 → 数据面 | SPSC | 拒绝写入 | **新增** |

### 2.2 为什么选这些原语？

**MetricsBuffer (覆盖旧数据)**:
- AI 只关心**最近**的采样，旧数据丢了没关系
- 数据面不能因为"缓冲区满"而等待——覆盖是唯一选择

**GlobalMetrics (atomic 计数器)**:
- 只有一个写者（数据面），`atomic<uint64_t>` increment 无竞争时等同普通 increment
- 智能面定期 `snapshot()` 一次拷贝出来，后续计算用普通变量

**SPSCQueue (拒绝满写)**:
- AI 决策队列很小（16 个就够），因为数据面每 10ms 消费一个
- 满了说明数据面还没消费完上一批，丢掉新决策是安全的

## 3. SPSCQueue 设计

### 3.1 与 MetricsBuffer 的对比

```
MetricsBuffer (采样缓冲区):        SPSCQueue (命令队列):
┌───┬───┬───┬───┬───┬───┐         ┌───┬───┬───┬───┐
│ 5 │ 6 │ 3 │ 4 │   │   │         │ A │ B │   │   │
└───┴───┴───┴───┴───┴───┘         └───┴───┴───┴───┘
      ↑ write                       ↑ read    ↑ write
  满了? 覆盖最旧的!                  满了? 返回 false!
  没有 read 指针                     有 read 指针 (消费后前移)
```

### 3.2 接口

```cpp
// 新文件: include/neustack/common/spsc_queue.hpp

template <typename T, size_t N>  // N 必须是 2 的幂
class SPSCQueue {
public:
    // 生产者 (智能面线程)
    bool try_push(const T& item);   // 成功 true, 满了 false

    // 消费者 (数据面线程)
    bool try_pop(T& item);          // 成功 true, 空了 false

    bool empty() const;
    size_t size() const;
};
```

### 3.3 实现原理

```
内存布局:  [  slot0  |  slot1  |  slot2  |  slot3  ]
                                  ↑ read     ↑ write

try_push(X):
  1. 读 write_pos (relaxed)
  2. 读 read_pos (acquire) ← 确保看到消费者的最新进度
  3. 如果 write - read >= N → 满了, return false
  4. buffer[write & (N-1)] = X
  5. write_pos.store(write+1, release) ← 确保数据对消费者可见

try_pop(X):
  1. 读 read_pos (relaxed)
  2. 读 write_pos (acquire) ← 确保看到生产者的最新数据
  3. 如果 read == write → 空的, return false
  4. X = buffer[read & (N-1)]
  5. read_pos.store(read+1, release) ← 确保生产者看到空间释放

关键: 只用 memory_order_acquire/release，不用 seq_cst
     这在 x86 上编译为普通 mov，零额外开销
```

### 3.4 完整代码

```cpp
// include/neustack/common/spsc_queue.hpp

#ifndef NEUSTACK_COMMON_SPSC_QUEUE_HPP
#define NEUSTACK_COMMON_SPSC_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace neustack {

template <typename T, size_t N>
class SPSCQueue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SPSCQueue element must be trivially copyable");
    static_assert((N & (N - 1)) == 0,
                  "SPSCQueue size must be power of 2");

public:
    SPSCQueue() : _read_pos(0), _write_pos(0) {}

    /**
     * 生产者: 尝试写入 (智能面线程调用)
     */
    bool try_push(const T& item) {
        size_t write = _write_pos.load(std::memory_order_relaxed);
        size_t read = _read_pos.load(std::memory_order_acquire);

        if (write - read >= N) return false;  // 满

        _buffer[write & (N - 1)] = item;
        _write_pos.store(write + 1, std::memory_order_release);
        return true;
    }

    /**
     * 消费者: 尝试读取 (数据面线程调用)
     */
    bool try_pop(T& item) {
        size_t read = _read_pos.load(std::memory_order_relaxed);
        size_t write = _write_pos.load(std::memory_order_acquire);

        if (read == write) return false;  // 空

        item = _buffer[read & (N - 1)];
        _read_pos.store(read + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return _read_pos.load(std::memory_order_acquire)
            == _write_pos.load(std::memory_order_acquire);
    }

    size_t size() const {
        return _write_pos.load(std::memory_order_acquire)
             - _read_pos.load(std::memory_order_acquire);
    }

private:
    std::array<T, N> _buffer;
    // 分开放，避免 false sharing (不同 cache line)
    alignas(64) std::atomic<size_t> _read_pos;
    alignas(64) std::atomic<size_t> _write_pos;
};

} // namespace neustack

#endif // NEUSTACK_COMMON_SPSC_QUEUE_HPP
```

> **操作系统知识点: False Sharing**
>
> `_read_pos` 和 `_write_pos` 用 `alignas(64)` 放在不同的 cache line 上。
> 如果它们在同一个 cache line (64 bytes)，数据面写 `read_pos` 时会使
> 智能面缓存的 `write_pos` 失效，反之亦然——这叫 **false sharing**，
> 会导致大量的缓存一致性流量，严重降低性能。

## 4. AI 动作定义

数据面需要知道智能面传回来什么：

```cpp
// include/neustack/metrics/ai_action.hpp

#ifndef NEUSTACK_METRICS_AI_ACTION_HPP
#define NEUSTACK_METRICS_AI_ACTION_HPP

#include <cstdint>

namespace neustack {

/**
 * 智能面 → 数据面的 AI 决策
 *
 * 通过 SPSCQueue 传递，必须是 trivially copyable
 */
struct AIAction {
    enum class Type : uint8_t {
        NONE = 0,
        CWND_ADJUST,      // Orca: 调整拥塞窗口
        ANOMALY_ALERT,    // 异常检测: 发现异常
        BW_PREDICTION,    // 带宽预测: 更新预测值
    };

    Type type = Type::NONE;
    uint32_t conn_id = 0;    // 目标连接 (0 = 全局)

    union {
        // CWND_ADJUST
        struct {
            float alpha;          // cwnd_new = 2^alpha * cwnd_cubic
        } cwnd;

        // ANOMALY_ALERT
        struct {
            float score;          // 异常分数 [0, 1]
        } anomaly;

        // BW_PREDICTION
        struct {
            uint32_t predicted_bw; // 预测带宽 (bytes/s)
        } bandwidth;
    };
};

// 编译期检查
static_assert(sizeof(AIAction) <= 16, "AIAction should be compact");

} // namespace neustack

#endif // NEUSTACK_METRICS_AI_ACTION_HPP
```

## 5. 智能面线程骨架

```cpp
// include/neustack/ai/intelligence_plane.hpp (骨架)

#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/ai_action.hpp"
#include <thread>
#include <atomic>

namespace neustack {

class IntelligencePlane {
public:
    IntelligencePlane(
        MetricsBuffer<TCPSample, 1024>& metrics_buf,  // 数据面 → 读
        SPSCQueue<AIAction, 16>& action_queue           // 写 → 数据面
    )
        : _metrics_buf(metrics_buf)
        , _action_queue(action_queue)
        , _running(false)
    {}

    void start() {
        _running = true;
        _thread = std::thread([this] { run(); });
    }

    void stop() {
        _running = false;
        if (_thread.joinable()) _thread.join();
    }

private:
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    SPSCQueue<AIAction, 16>& _action_queue;
    std::atomic<bool> _running;
    std::thread _thread;

    void run() {
        GlobalMetrics::Snapshot prev_snapshot = {};

        while (_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // ─── 1. 读取采样数据 ───
            auto samples = _metrics_buf.recent(100);

            // ─── 2. 读取全局统计快照 ───
            auto snapshot = global_metrics().snapshot();
            auto delta = snapshot.diff(prev_snapshot);
            prev_snapshot = snapshot;

            // ─── 3. AI 推理 (后续教程实现) ───
            // auto features = extract_features(samples);
            // auto action = onnx_infer(features);

            // ─── 4. 传回数据面 ───
            // _action_queue.try_push(action);
        }
    }
};

} // namespace neustack
```

## 6. 数据面集成

数据面需要持有共享的通信通道：

```cpp
// tcp_layer 中:

class TCPLayer {
    // ... 现有字段 ...

    // ─── AI 通信通道 ───
    MetricsBuffer<TCPSample, 1024> _metrics_buf;  // → 智能面
    SPSCQueue<AIAction, 16> _action_queue;          // ← 智能面

    // ─── 智能面线程 ───
    IntelligencePlane _ai;

    // 启动
    void start() {
        _ai = IntelligencePlane(_metrics_buf, _action_queue);
        _ai.start();
    }

    // 在事件循环中检查 AI 决策
    void tick() {
        AIAction action;
        while (_action_queue.try_pop(action)) {
            switch (action.type) {
                case AIAction::Type::CWND_ADJUST:
                    apply_cwnd_action(action);
                    break;
                case AIAction::Type::ANOMALY_ALERT:
                    handle_anomaly(action);
                    break;
                default:
                    break;
            }
        }
    }
};
```

## 7. 完整数据流

```
一个包的完整生命周期:

  1. NIC → HAL.receive()
  2. IPv4.handle() → TCP.handle()
  3. process_ack() {
       update_rtt();
       update_cwnd();
       metrics_buf.push(sample);      ← 无锁写入，O(1)
       global_metrics().packets_rx++; ← 普通 atomic inc，无竞争
     }
  4. tcp_layer.tick() {
       action_queue.try_pop(action);  ← 无锁读取，O(1)
       if (action) apply(action);
     }
  5. 回到 1

  与此同时，在另一个核上:

  智能面 Thread:
  6. sleep(10ms)
  7. samples = metrics_buf.recent(100);   ← 无锁读取
  8. snapshot = global_metrics().snapshot(); ← atomic load
  9. features = extract(samples);
  10. action = onnx_infer(features);       ← 0.1-1ms, 不阻塞数据面!
  11. action_queue.try_push(action);        ← 无锁写入
  12. 回到 6
```

## 8. 需要新增的文件

```
include/neustack/common/
├── ring_buffer.hpp        # MetricsBuffer, StreamBuffer (已有)
└── spsc_queue.hpp         # SPSCQueue (新增)

include/neustack/metrics/
├── tcp_sample.hpp         # TCPSample (已有)
├── global_metrics.hpp     # GlobalMetrics (已有)
├── ai_features.hpp        # OrcaFeatures 等 (已有)
└── ai_action.hpp          # AIAction (新增)

include/neustack/ai/
└── intelligence_plane.hpp # 智能面线程 (新增)
```

## 9. 下一步

本教程建立了双线程架构的通信基础。下一步：

- **02_metrics_collection.md** - 定义采集点，在 TCP 关键路径埋入 push()
- **03_onnx_integration.md** - 在智能面线程中加载 ONNX 模型

## 10. 参考资料

### 无锁编程
- [C++ memory order](https://en.cppreference.com/w/cpp/atomic/memory_order)
- [Preshing: Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
- [False Sharing](https://mechanical-sympathy.blogspot.com/2011/07/false-sharing.html)

### 用户态协议栈架构
- [DPDK Run-to-Completion](https://doc.dpdk.org/guides/prog_guide/overview.html)
- [Seastar Shared-Nothing](https://seastar.io/shared-nothing/)
- [mTCP](https://github.com/mtcp-stack/mtcp) - 多核用户态 TCP
