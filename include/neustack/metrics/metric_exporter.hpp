#ifndef NEUSTACK_METRICS_EXPORTER_HPP
#define NEUSTACK_METRICS_EXPORTER_HPP

#include "neustack/metrics/global_metrics.hpp"
#include <fstream>
#include <chrono>

namespace neustack {

/**
 * 指标导出器 - 定期将 GlobalMetrics 快照导出到 CSV
 */
class MetricsExporter {
public:
    explicit MetricsExporter(const std::string& filepath)
        : _file(filepath), _prev_snapshot(global_metrics().snapshot())
    {
        // 写入 CSV 头
        _file << "timestamp_ms,packets_rx,packets_tx,bytes_rx,bytes_tx,"
              << "syn_received,rst_received,conn_established,conn_reset,"
              << "active_connections\n";
    }

    ~MetricsExporter() {
        _file.close();
    }

    /**
     * 导出当前快照 (计算与上次的差值)
     * @param interval_ms 采样间隔 (毫秒)
     */
    void export_delta(uint64_t interval_ms) {
        auto snapshot = global_metrics().snapshot();
        auto delta = snapshot.diff(_prev_snapshot);

        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        // 导出 delta 值（累计计数器的变化量）和瞬时值（active_connections）
        _file << ms << ","
              << delta.packets_rx << ","
              << delta.packets_tx << ","
              << delta.bytes_rx << ","
              << delta.bytes_tx << ","
              << delta.syn_received << ","
              << delta.rst_received << ","
              << delta.conn_established << ","
              << delta.conn_reset << ","
              << snapshot.active_connections << "\n";

        _prev_snapshot = snapshot;
    }

    void flush() { _file.flush(); }

private:
    std::ofstream _file;
    GlobalMetrics::Snapshot _prev_snapshot;
};

} // namespace neustack

#endif