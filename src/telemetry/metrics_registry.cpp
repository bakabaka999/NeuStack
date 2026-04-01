#include "neustack/telemetry/metrics_registry.hpp"

void neustack::telemetry::register_builtin_metrics(GlobalMetrics &gm, const SecurityMetrics *sm) {
    auto& reg = MetricsRegistry::instance();

    // ─── 包统计 (桥接 GlobalMetrics atomic counters) ───
    reg.bridge_gauge("neustack_packets_rx_total",
        "Total packets received",
        [&gm]() { return static_cast<double>(gm.packets_rx.load(std::memory_order_relaxed)); },
        "packets");

    reg.bridge_gauge("neustack_packets_tx_total",
        "Total packets transmitted",
        [&gm]() { return static_cast<double>(gm.packets_tx.load(std::memory_order_relaxed)); },
        "packets");

    reg.bridge_gauge("neustack_bytes_rx_total",
        "Total bytes received",
        [&gm]() { return static_cast<double>(gm.bytes_rx.load(std::memory_order_relaxed)); },
        "bytes");

    reg.bridge_gauge("neustack_bytes_tx_total",
        "Total bytes transmitted",
        [&gm]() { return static_cast<double>(gm.bytes_tx.load(std::memory_order_relaxed)); },
        "bytes");

    // ─── 连接统计 ───
    reg.bridge_gauge("neustack_tcp_active_connections",
        "Current active TCP connections",
        [&gm]() { return static_cast<double>(gm.active_connections.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_connections_established_total",
        "Total TCP connections established",
        [&gm]() { return static_cast<double>(gm.conn_established.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_connections_reset_total",
        "Total TCP connections reset",
        [&gm]() { return static_cast<double>(gm.conn_reset.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_connections_timeout_total",
        "Total TCP connections timed out",
        [&gm]() { return static_cast<double>(gm.conn_timeout.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_retransmits_total",
        "Total TCP retransmissions",
        [&gm]() { return static_cast<double>(gm.total_retransmits.load(std::memory_order_relaxed)); });

    // ─── TCP 标志统计 ───
    reg.bridge_gauge("neustack_tcp_syn_received_total",
        "Total SYN packets received",
        [&gm]() { return static_cast<double>(gm.syn_received.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_rst_received_total",
        "Total RST packets received",
        [&gm]() { return static_cast<double>(gm.rst_received.load(std::memory_order_relaxed)); });

    reg.bridge_gauge("neustack_tcp_fin_received_total",
        "Total FIN packets received",
        [&gm]() { return static_cast<double>(gm.fin_received.load(std::memory_order_relaxed)); });

    // ─── RTT 分布 (新增 Histogram) ───
    // 桶: 50us, 100us, 200us, 500us, 1ms, 2ms, 5ms, 10ms, 50ms, 100ms, 500ms
    reg.histogram("neustack_tcp_rtt_us",
        "TCP round-trip time distribution",
        {50, 100, 200, 500, 1000, 2000, 5000, 10000, 50000, 100000, 500000},
        "microseconds");

    // ─── 安全指标 (桥接 SecurityMetrics) ───
    if (sm) {
        reg.bridge_gauge("neustack_security_pps",
            "Current packets per second (security view)",
            [sm]() { return sm->snapshot().pps; });

        reg.bridge_gauge("neustack_security_syn_rate",
            "Current SYN packets per second",
            [sm]() { return sm->snapshot().syn_rate; });

        reg.bridge_gauge("neustack_security_syn_synack_ratio",
            "SYN to SYN-ACK ratio (high = possible SYN flood)",
            [sm]() { return sm->snapshot().syn_to_synack_ratio; });

        reg.bridge_gauge("neustack_security_rst_ratio",
            "RST to total packets ratio",
            [sm]() { return sm->snapshot().rst_ratio; });

        reg.bridge_gauge("neustack_security_dropped_total",
            "Total packets dropped by firewall",
            [sm]() { return static_cast<double>(sm->snapshot().dropped_packets); },
            "packets");

        reg.bridge_gauge("neustack_security_alerted_total",
            "Total packets that triggered alerts",
            [sm]() { return static_cast<double>(sm->snapshot().alerted_packets); },
            "packets");
    }
}
