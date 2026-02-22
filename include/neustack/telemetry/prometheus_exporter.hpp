#ifndef NEUSTACK_TELEMETRY_PROMETHEUS_EXPORTER_HPP
#define NEUSTACK_TELEMETRY_PROMETHEUS_EXPORTER_HPP

#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/telemetry/exporter.hpp"
#include <string>

namespace neustack::telemetry {
    
class PrometheusExporter : public Exporter {
public:
    // ─── Exporter 接口实现 ───

    std::string format_name() const override { return "prometheus"; }

    std::string content_type() const override { return "text/plain; version=0.0.4; charset=utf-8"; }

    /**
     * 将 Registry 中所有指标序列化为 Prometheus 文本格式
     */
    std::string serialize(const MetricsRegistry &registry) const override;

private:
    // HELP 行中的转义: \ → \\, \n → \\n
    static void escape_help(std::string &out, std::string_view in);

    // 格式化数值 (Prometheus 格式)
    // 整数: 不带小数点
    // 浮点: 用 %.17g 保证精度 (Prometheus 建议)
    // 特殊值: +Inf, -Inf, NaN (Prometheus 原生支持)
    static void format_value(std::string &out, double v);
    static void format_value(std::string &out, uint64_t v);

    // 写一个完整 metric 块 (HELP + TYPE + data lines)
    static void write_counter(std::string &out, const Counter &c);
    static void write_gauge(std::string &out, const Gauge &g);
    static void write_bridge_gauge(std::string &out,
                                   const MetricsRegistry::BridgeGauge &bg);
    static void write_histogram(std::string &out, const Histogram &h);
};

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_PROMETHEUS_EXPORTER_HPP