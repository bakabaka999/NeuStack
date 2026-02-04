#ifndef NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP
#define NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP

#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/common/ring_buffer.hpp"
#include <fstream>
#include <string>

namespace neustack {

/**
 * TCPSample CSV 导出器
 *
 * 定期从 MetricsBuffer 读取新样本，追加写入 CSV 文件。
 * 用于离线训练数据采集。
 */
class SampleExporter {
public:
    explicit SampleExporter(
        const std::string& filepath,
        MetricsBuffer<TCPSample, 1024>& metrics_buf
    )
        : _file(filepath)
        , _metrics_buf(metrics_buf)
        , _last_total(0)
    {
        // CSV 头
        _file << "timestamp_us,"
              << "rtt_us,min_rtt_us,srtt_us,"
              << "cwnd,ssthresh,bytes_in_flight,"
              << "delivery_rate,send_rate,"
              << "loss_detected,timeout_occurred,ecn_ce_count,"
              << "is_app_limited,packets_sent,packets_lost\n";
    }

    /**
     * 导出新增的样本
     *
     * 通过比较 total_pushed() 检测是否有新数据。
     * 典型调用频率: 每 100ms 或每 1s。
     */
    void export_new_samples() {
        size_t current_total = _metrics_buf.total_pushed();
        if (current_total == _last_total) return;  // 无新数据

        // 读取最近的样本
        size_t new_count = current_total - _last_total;
        auto samples = _metrics_buf.recent(
            std::min(new_count, static_cast<size_t>(1024))
        );

        for (const auto& s : samples) {
            // 跳过已经导出过的 (按时间戳去重)
            if (s.timestamp_us <= _last_timestamp) continue;

            _file << s.timestamp_us << ","
                  << s.rtt_us << "," << s.min_rtt_us << "," << s.srtt_us << ","
                  << s.cwnd << "," << s.ssthresh << "," << s.bytes_in_flight << ","
                  << s.delivery_rate << "," << s.send_rate << ","
                  << static_cast<int>(s.loss_detected) << ","
                  << static_cast<int>(s.timeout_occurred) << ","
                  << static_cast<int>(s.ecn_ce_count) << ","
                  << static_cast<int>(s.is_app_limited) << ","
                  << s.packets_sent << "," << s.packets_lost << "\n";

            _last_timestamp = s.timestamp_us;
            _exported_count++;
        }

        _last_total = current_total;
    }

    void flush() { _file.flush(); }
    size_t exported_count() const { return _exported_count; }

private:
    std::ofstream _file;
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    size_t _last_total;
    uint64_t _last_timestamp = 0;
    size_t _exported_count = 0;
};

} // namespace neustack

#endif // NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP