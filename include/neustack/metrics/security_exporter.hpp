#ifndef NEUSTACK_METRICS_SECURITY_EXPORTER_HPP
#define NEUSTACK_METRICS_SECURITY_EXPORTER_HPP

#include "neustack/metrics/security_metrics.hpp"
#include <fstream>
#include <string>
#include <chrono>
#include <cstdint>

namespace neustack {

/**
 * 安全指标 CSV 导出器 — 为 AI 模型训练采集数据
 *
 * 使用方式：
 *   SecurityExporter exporter("security_data.csv", metrics);
 *   // 每秒调用一次
 *   exporter.flush();           // label = 0 (正常)
 *   exporter.flush(1);          // label = 1 (异常，手动标注)
 *
 * CSV 列：
 *   timestamp_ms, packets_total, bytes_total,
 *   syn_packets, syn_ack_packets, rst_packets,
 *   pps, bps, syn_rate, rst_rate,
 *   syn_ratio, new_conn_rate, avg_pkt_size, rst_ratio,
 *   label
 *
 * 设计原则：
 * - 与 SampleExporter 风格一致
 * - 记录原始值 + 衍生速率，Python 端负责归一化
 * - 低开销：只在 flush() 时读快照
 */
class SecurityExporter {
public:
    /**
     * @param filepath  输出 CSV 路径
     * @param metrics   SecurityMetrics 引用（不持有所有权）
     */
    explicit SecurityExporter(const std::string& filepath,
                              const SecurityMetrics& metrics)
        : _metrics(metrics)
        , _file(filepath, std::ios::app)
    {
        if (!_file.is_open()) return;

        // 如果文件为空，写 header
        _file.seekp(0, std::ios::end);
        if (_file.tellp() == 0) {
            write_header();
        }
    }

    /**
     * 采样当前快照并追加一行
     * @param label 0=正常, 1=异常 (手动/自动标注)
     */
    void flush(int label = 0) {
        if (!_file.is_open()) return;

        auto snap = _metrics.snapshot();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // 衍生指标
        double avg_pkt_size = (snap.pps > 0) ? (snap.bps / snap.pps) : 0.0;
        double rst_ratio = (snap.pps > 0) ? (snap.rst_rate / snap.pps) : 0.0;
        double syn_ratio = snap.syn_to_synack_ratio; // 已由 SecurityMetrics 计算

        _file << now_ms << ','
              << snap.packets_total << ','
              << snap.bytes_total << ','
              << snap.syn_packets << ','
              << snap.syn_ack_packets << ','
              << snap.rst_packets << ','
              << snap.pps << ','
              << snap.bps << ','
              << snap.syn_rate << ','
              << snap.rst_rate << ','
              << syn_ratio << ','
              << snap.new_conn_rate << ','
              << avg_pkt_size << ','
              << rst_ratio << ','
              << label << '\n';

        _rows_written++;

        // 每 100 行 flush 一次磁盘
        if (_rows_written % 100 == 0) {
            _file.flush();
        }
    }

    /// 强制刷盘
    void sync() { _file.flush(); }

    /// 已写入行数
    uint64_t rows_written() const { return _rows_written; }

    /// 文件是否可用
    bool is_open() const { return _file.is_open(); }

private:
    const SecurityMetrics& _metrics;
    std::ofstream _file;
    uint64_t _rows_written = 0;

    void write_header() {
        _file << "timestamp_ms,"
              << "packets_total,bytes_total,"
              << "syn_packets,syn_ack_packets,rst_packets,"
              << "pps,bps,syn_rate,rst_rate,"
              << "syn_ratio,new_conn_rate,avg_pkt_size,rst_ratio,"
              << "label\n";
    }
};

} // namespace neustack

#endif // NEUSTACK_METRICS_SECURITY_EXPORTER_HPP
