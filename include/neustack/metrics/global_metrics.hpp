#ifndef NEUSTACK_METRICS_GLOBAL_METRICS_HPP
#define NEUSTACK_METRICS_GLOBAL_METRICS_HPP

#include <cstdint>
#include <atomic>

namespace neustack {

/**
 * 全局统计指标
 *
 * 跨线程访问模式:
 *   - 数据面线程: 写 (increment)
 *   - 智能面线程: 读 (每秒快照)
 *
 * 使用 atomic 是因为架构 B 中智能面在独立线程。
 * 注意: 无竞争的 atomic increment 开销极小（≈ 普通 increment），
 * 因为只有一个写者，cache line 不会 bouncing。
 */
struct GlobalMetrics {
    // ─── 包统计 ───
    std::atomic<uint64_t> packets_rx{0};
    std::atomic<uint64_t> packets_tx{0};
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> bytes_tx{0};

    // ─── TCP 标志统计 (异常检测核心) ───
    std::atomic<uint64_t> syn_received{0};
    std::atomic<uint64_t> syn_ack_sent{0};
    std::atomic<uint64_t> rst_received{0};
    std::atomic<uint64_t> rst_sent{0};
    std::atomic<uint64_t> fin_received{0};

    // ─── 连接统计 ───
    std::atomic<uint32_t> active_connections{0};
    std::atomic<uint64_t> conn_established{0};
    std::atomic<uint64_t> conn_closed{0};
    std::atomic<uint64_t> conn_reset{0};
    std::atomic<uint64_t> conn_timeout{0};

    // ─── 快照 (智能面读取用) ───

    /**
     * 非原子快照结构体 (用于智能面内部计算)
     *
     * 智能面线程每秒调用 snapshot()，拿到普通结构体后
     * 可以在本线程内随意计算，不再涉及原子操作。
     */
    struct Snapshot {
        uint64_t packets_rx;
        uint64_t packets_tx;
        uint64_t bytes_rx;
        uint64_t bytes_tx;
        uint64_t syn_received;
        uint64_t rst_received;
        uint64_t fin_received;
        uint32_t active_connections;
        uint64_t conn_established;
        uint64_t conn_closed;
        uint64_t conn_reset;
        uint64_t conn_timeout;

        // 与上一次快照的差值 (用于计算速率)
        struct Delta {
            uint64_t packets_rx;
            uint64_t packets_tx;
            uint64_t bytes_rx;
            uint64_t bytes_tx;
            uint64_t syn_received;
            uint64_t rst_received;
            uint64_t conn_established;
            uint64_t conn_reset;
        };

        Delta diff(const Snapshot& prev) const {
            return {
                .packets_rx = packets_rx - prev.packets_rx,
                .packets_tx = packets_tx - prev.packets_tx,
                .bytes_rx = bytes_rx - prev.bytes_rx,
                .bytes_tx = bytes_tx - prev.bytes_tx,
                .syn_received = syn_received - prev.syn_received,
                .rst_received = rst_received - prev.rst_received,
                .conn_established = conn_established - prev.conn_established,
                .conn_reset = conn_reset - prev.conn_reset,
            };
        }
    };

    /**
     * 创建快照 (智能面线程调用)
     *
     * 每个字段单独 load，不是全局一致的快照，
     * 但对于统计聚合来说足够准确。
     */
    Snapshot snapshot() const {
        return {
            .packets_rx = packets_rx.load(std::memory_order_relaxed),
            .packets_tx = packets_tx.load(std::memory_order_relaxed),
            .bytes_rx = bytes_rx.load(std::memory_order_relaxed),
            .bytes_tx = bytes_tx.load(std::memory_order_relaxed),
            .syn_received = syn_received.load(std::memory_order_relaxed),
            .rst_received = rst_received.load(std::memory_order_relaxed),
            .fin_received = fin_received.load(std::memory_order_relaxed),
            .active_connections = active_connections.load(std::memory_order_relaxed),
            .conn_established = conn_established.load(std::memory_order_relaxed),
            .conn_closed = conn_closed.load(std::memory_order_relaxed),
            .conn_reset = conn_reset.load(std::memory_order_relaxed),
            .conn_timeout = conn_timeout.load(std::memory_order_relaxed),
        };
    }
};

// 全局单例
inline GlobalMetrics& global_metrics() {
    static GlobalMetrics instance;
    return instance;
}

} // namespace neustack

#endif // NEUSTACK_METRICS_GLOBAL_METRICS_HPP
