#ifndef NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP
#define NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP

#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/telemetry/exporter.hpp"
#include <string>
#include <cstdint>

namespace neustack::telemetry {
    
class JsonExporter : public Exporter {
public:
    /**
     * @param pretty 是否格式化输出 (增加约 30% 体积)
     */
    explicit JsonExporter(bool pretty = false) : _pretty(pretty) {}

    // ─── Exporter 接口实现 ───

    std::string format_name() const override { return "json"; }
    
    std::string content_type() const override { return "application/json"; }

    /**
     * 将 Registry 中所有指标序列化为 JSON 字符串
     */
    std::string serialize(const MetricsRegistry& registry) const override;

    // ─── JSON 专有接口 ───

    /**
     * 只导出指定前缀的指标
     * e.g. "neustack_tcp"
     */
    std::string serialize_filtered(const MetricsRegistry& registry,
                                   const std::string& prefix) const;
private:
    bool _pretty;

    // ─── 内部 JSON 构建工具 ───
    struct JsonBuilder
    {
        std::string buf;
        bool pretty;
        int depth;

        explicit JsonBuilder(bool pretty_print, size_t reserve = 4096)
            : pretty(pretty_print), depth(0)
        {
            buf.reserve(reserve);
        }

        void indent();
        void newline();
        void write_string(std::string_view s);
        void write_uint64(uint64_t v);
        void write_double(double v);
        void write_raw(std::string_view s);
        void begin_object();
        void end_object();
        void begin_array();
        void end_array();
        void key(std::string_view k);
        void comma();
    };

    // JSON 字符串转义
    static void escape_string(std::string &out, std::string_view in);

    // 格式化数值
    static void format_uint64(std::string &out, uint64_t v);
    static void format_double(std::string &out, double v);

    // Histogram percentile 计算
    static double estimate_percentile(const Histogram::Snapshot &snap, double q);

    // 内部序列化各类型
    static void serialize_counter(JsonBuilder &b, const Counter &c);
    static void serialize_gauge(JsonBuilder &b, const Gauge &g);
    static void serialize_bridge(JsonBuilder &b, const MetricsRegistry::BridgeGauge &bg);
    static void serialize_histogram(JsonBuilder &b, const Histogram &h);
};

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_JSON_EXPORTER_HPP