#ifndef NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP
#define NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP

#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/telemetry/exporter.hpp"
#include "neustack/common/json_builder.hpp"
#include <string>

namespace neustack::telemetry {

class JsonExporter : public Exporter {
public:
    explicit JsonExporter(bool pretty = false) : _pretty(pretty) {}

    std::string format_name() const override { return "json"; }
    std::string content_type() const override { return "application/json"; }

    /** 将 Registry 中所有指标序列化为 JSON 字符串 */
    std::string serialize(const MetricsRegistry& registry) const override;

    /** 只导出指定前缀的指标，e.g. "neustack_tcp" */
    std::string serialize_filtered(const MetricsRegistry& registry,
                                   const std::string& prefix) const;

    // 各类型序列化（供内部和 write_metrics_object 使用）
    static void serialize_counter  (neustack::JsonBuilder& b, const Counter& c);
    static void serialize_gauge    (neustack::JsonBuilder& b, const Gauge& g);
    static void serialize_bridge   (neustack::JsonBuilder& b, const MetricsRegistry::BridgeGauge& bg);
    static void serialize_histogram(neustack::JsonBuilder& b, const Histogram& h);

private:
    bool _pretty;

    static double estimate_percentile(const Histogram::Snapshot& snap, double q);
};

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP
