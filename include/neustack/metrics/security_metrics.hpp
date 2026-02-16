#ifndef NEUSTACK_METRICS_SECURITY_METRICS_HPP
#define NEUSTACK_METRICS_SECURITY_METRICS_HPP

#include <cstdint>
#include <atomic>
#include <array>
#include <chrono>

namespace neustack {

/**
 * 安全指标 - 防火墙专用的流量统计
 *
 * 设计原则:
 * - 使用滑动窗口（默认 10 秒）计算速率，比瞬时值更稳定
 * - 原子操作保证线程安全（数据面写，AI 面读）
 * - 支持快照和差值计算，便于特征提取
 *
 * 与 GlobalMetrics 的区别:
 * - GlobalMetrics 是累积计数器，需要外部计算差值
 * - SecurityMetrics 内置滑动窗口，直接提供速率
 *
 * 线程模型:
 * - 数据面线程: 调用 record_*() 方法
 * - AI 线程/定时器: 调用 tick() 更新窗口，snapshot() 获取数据
 */
class SecurityMetrics {
public:
    // ─── 配置常量 ───
    static constexpr size_t WINDOW_SIZE = 10;      // 滑动窗口大小（秒）
    static constexpr size_t TICK_INTERVAL_MS = 1000; // tick 间隔（毫秒）

    // ─── 当前窗口的原子计数器 ───
    struct Counters {
        std::atomic<uint64_t> packets_total{0};    // 总包数
        std::atomic<uint64_t> bytes_total{0};      // 总字节数
        
        std::atomic<uint64_t> syn_packets{0};      // SYN 包数
        std::atomic<uint64_t> syn_ack_packets{0};  // SYN-ACK 包数
        std::atomic<uint64_t> rst_packets{0};      // RST 包数
        std::atomic<uint64_t> fin_packets{0};      // FIN 包数
        
        std::atomic<uint64_t> new_connections{0};  // 新建连接数 (收到 SYN)
        std::atomic<uint64_t> half_open{0};        // 半开连接数 (SYN 未完成握手)
        
        std::atomic<uint64_t> dropped_packets{0};  // 被防火墙丢弃的包
        std::atomic<uint64_t> alerted_packets{0};  // 触发告警的包
        
        void reset() {
            packets_total.store(0, std::memory_order_relaxed);
            bytes_total.store(0, std::memory_order_relaxed);
            syn_packets.store(0, std::memory_order_relaxed);
            syn_ack_packets.store(0, std::memory_order_relaxed);
            rst_packets.store(0, std::memory_order_relaxed);
            fin_packets.store(0, std::memory_order_relaxed);
            new_connections.store(0, std::memory_order_relaxed);
            half_open.store(0, std::memory_order_relaxed);
            dropped_packets.store(0, std::memory_order_relaxed);
            alerted_packets.store(0, std::memory_order_relaxed);
        }
    };

    // ─── 快照结构（非原子，用于 AI 计算）───
    struct Snapshot {
        // 当前秒的计数
        uint64_t packets_total;
        uint64_t bytes_total;
        uint64_t syn_packets;
        uint64_t syn_ack_packets;
        uint64_t rst_packets;
        uint64_t fin_packets;
        uint64_t new_connections;
        uint64_t half_open;
        uint64_t dropped_packets;
        uint64_t alerted_packets;
        
        // 滑动窗口内的速率（per second）
        double pps;              // Packets Per Second
        double bps;              // Bytes Per Second
        double syn_rate;         // SYN 包速率
        double rst_rate;         // RST 包速率
        double new_conn_rate;    // 新连接速率
        
        // 比率指标
        double syn_to_synack_ratio;  // SYN/(SYN-ACK)，高值可能是 SYN Flood
        double rst_ratio;            // RST/总包，高值可能是端口扫描
        double avg_packet_size;      // 平均包大小
    };

    // ─── 构造函数 ───
    SecurityMetrics() {
        _current.reset();
        for (auto& slot : _window) {
            slot = {};
        }
    }

    // ─── 数据面调用：记录事件 ───

    /** 记录收到的数据包 */
    void record_packet(uint16_t len, uint8_t tcp_flags = 0) {
        _current.packets_total.fetch_add(1, std::memory_order_relaxed);
        _current.bytes_total.fetch_add(len, std::memory_order_relaxed);
        
        // 解析 TCP 标志
        if (tcp_flags != 0) {
            constexpr uint8_t TCP_SYN = 0x02;
            constexpr uint8_t TCP_RST = 0x04;
            constexpr uint8_t TCP_FIN = 0x01;
            constexpr uint8_t TCP_ACK = 0x10;
            
            if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
                _current.syn_packets.fetch_add(1, std::memory_order_relaxed);
                _current.new_connections.fetch_add(1, std::memory_order_relaxed);
            }
            if ((tcp_flags & TCP_SYN) && (tcp_flags & TCP_ACK)) {
                _current.syn_ack_packets.fetch_add(1, std::memory_order_relaxed);
            }
            if (tcp_flags & TCP_RST) {
                _current.rst_packets.fetch_add(1, std::memory_order_relaxed);
            }
            if (tcp_flags & TCP_FIN) {
                _current.fin_packets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /** 记录丢弃的数据包 */
    void record_drop() {
        _current.dropped_packets.fetch_add(1, std::memory_order_relaxed);
    }

    /** 记录告警的数据包 */
    void record_alert() {
        _current.alerted_packets.fetch_add(1, std::memory_order_relaxed);
    }

    // ─── AI 面/定时器调用：更新窗口 ───

    /**
     * 滑动窗口前进一格
     * 
     * 应该每秒调用一次（由定时器或 AI 线程驱动）
     * 将当前计数器保存到窗口，然后重置
     */
    void tick() {
        // 保存当前窗口到历史
        _window[_window_index] = {
            .packets_total = _current.packets_total.load(std::memory_order_relaxed),
            .bytes_total = _current.bytes_total.load(std::memory_order_relaxed),
            .syn_packets = _current.syn_packets.load(std::memory_order_relaxed),
            .syn_ack_packets = _current.syn_ack_packets.load(std::memory_order_relaxed),
            .rst_packets = _current.rst_packets.load(std::memory_order_relaxed),
            .fin_packets = _current.fin_packets.load(std::memory_order_relaxed),
            .new_connections = _current.new_connections.load(std::memory_order_relaxed),
            .half_open = _current.half_open.load(std::memory_order_relaxed),
            .dropped_packets = _current.dropped_packets.load(std::memory_order_relaxed),
            .alerted_packets = _current.alerted_packets.load(std::memory_order_relaxed),
        };
        
        // 移动窗口索引
        _window_index = (_window_index + 1) % WINDOW_SIZE;
        _window_count = std::min(_window_count + 1, WINDOW_SIZE);
        
        // 重置当前计数器
        _current.reset();
    }

    /**
     * 获取快照（包含滑动窗口统计）
     *
     * 累积字段（packets_total 等）使用最近一个已完成窗口的值，
     * 而非 _current（_current 可能刚被 tick() 重置，或只积累了部分数据）。
     * 速率字段使用整个滑动窗口 + _current 计算。
     */
    Snapshot snapshot() const {
        Snapshot snap{};

        // 累积字段：使用最近一个已完成窗口 slot 的值
        // 这是最近一次 tick() 保存的完整 1 秒数据
        if (_window_count > 0) {
            // _window_index 指向下一个要写入的位置，
            // 所以最近写入的是 (_window_index - 1 + WINDOW_SIZE) % WINDOW_SIZE
            size_t last = (_window_index + WINDOW_SIZE - 1) % WINDOW_SIZE;
            const auto& latest = _window[last];
            snap.packets_total = latest.packets_total;
            snap.bytes_total = latest.bytes_total;
            snap.syn_packets = latest.syn_packets;
            snap.syn_ack_packets = latest.syn_ack_packets;
            snap.rst_packets = latest.rst_packets;
            snap.fin_packets = latest.fin_packets;
            snap.new_connections = latest.new_connections;
            snap.half_open = latest.half_open;
            snap.dropped_packets = latest.dropped_packets;
            snap.alerted_packets = latest.alerted_packets;
        } else {
            // 窗口还没数据，使用 _current
            snap.packets_total = _current.packets_total.load(std::memory_order_relaxed);
            snap.bytes_total = _current.bytes_total.load(std::memory_order_relaxed);
            snap.syn_packets = _current.syn_packets.load(std::memory_order_relaxed);
            snap.syn_ack_packets = _current.syn_ack_packets.load(std::memory_order_relaxed);
            snap.rst_packets = _current.rst_packets.load(std::memory_order_relaxed);
            snap.fin_packets = _current.fin_packets.load(std::memory_order_relaxed);
            snap.new_connections = _current.new_connections.load(std::memory_order_relaxed);
            snap.half_open = _current.half_open.load(std::memory_order_relaxed);
            snap.dropped_packets = _current.dropped_packets.load(std::memory_order_relaxed);
            snap.alerted_packets = _current.alerted_packets.load(std::memory_order_relaxed);
        }

        // 计算滑动窗口内的总和（用于速率计算）
        uint64_t total_packets = _current.packets_total.load(std::memory_order_relaxed);
        uint64_t total_bytes = _current.bytes_total.load(std::memory_order_relaxed);
        uint64_t total_syn = _current.syn_packets.load(std::memory_order_relaxed);
        uint64_t total_syn_ack = _current.syn_ack_packets.load(std::memory_order_relaxed);
        uint64_t total_rst = _current.rst_packets.load(std::memory_order_relaxed);
        uint64_t total_new_conn = _current.new_connections.load(std::memory_order_relaxed);

        for (size_t i = 0; i < _window_count; ++i) {
            const auto& slot = _window[i];
            total_packets += slot.packets_total;
            total_bytes += slot.bytes_total;
            total_syn += slot.syn_packets;
            total_syn_ack += slot.syn_ack_packets;
            total_rst += slot.rst_packets;
            total_new_conn += slot.new_connections;
        }

        // 计算速率（除以窗口秒数）
        double window_seconds = static_cast<double>(_window_count + 1);
        snap.pps = static_cast<double>(total_packets) / window_seconds;
        snap.bps = static_cast<double>(total_bytes) / window_seconds;
        snap.syn_rate = static_cast<double>(total_syn) / window_seconds;
        snap.rst_rate = static_cast<double>(total_rst) / window_seconds;
        snap.new_conn_rate = static_cast<double>(total_new_conn) / window_seconds;

        // 计算比率指标
        snap.syn_to_synack_ratio = (total_syn_ack > 0)
            ? static_cast<double>(total_syn) / static_cast<double>(total_syn_ack)
            : static_cast<double>(total_syn);

        snap.rst_ratio = (total_packets > 0)
            ? static_cast<double>(total_rst) / static_cast<double>(total_packets)
            : 0.0;

        snap.avg_packet_size = (total_packets > 0)
            ? static_cast<double>(total_bytes) / static_cast<double>(total_packets)
            : 0.0;

        return snap;
    }

    // ─── 访问器 ───
    
    const Counters& current() const { return _current; }
    size_t window_count() const { return _window_count; }

private:
    // 当前秒的计数器
    Counters _current;
    
    // 滑动窗口历史
    struct WindowSlot {
        uint64_t packets_total = 0;
        uint64_t bytes_total = 0;
        uint64_t syn_packets = 0;
        uint64_t syn_ack_packets = 0;
        uint64_t rst_packets = 0;
        uint64_t fin_packets = 0;
        uint64_t new_connections = 0;
        uint64_t half_open = 0;
        uint64_t dropped_packets = 0;
        uint64_t alerted_packets = 0;
    };
    
    std::array<WindowSlot, WINDOW_SIZE> _window;
    size_t _window_index = 0;   // 下一个要写入的位置
    size_t _window_count = 0;   // 窗口中有效数据的数量
};

} // namespace neustack

#endif // NEUSTACK_METRICS_SECURITY_METRICS_HPP
