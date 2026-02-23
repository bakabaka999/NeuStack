#include "neustack/telemetry/prometheus_exporter.hpp"
#include <cstdio>
#include <cmath>

using namespace neustack::telemetry;

// ════════════════════════════════════════════
// 转义和格式化
// ════════════════════════════════════════════

void PrometheusExporter::escape_help(std::string& out, std::string_view in) {
    for (char c : in) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            default:   out.push_back(c);   break;
        }
    }
}

void PrometheusExporter::format_value(std::string& out, uint64_t v) {
    char tmp[24];
    int len = std::snprintf(tmp, sizeof(tmp), "%llu",
                            static_cast<unsigned long long>(v));
    out.append(tmp, static_cast<size_t>(len));
}

void PrometheusExporter::format_value(std::string& out, double v) {
    if (std::isnan(v)) {
        out.append("NaN");
        return;
    }
    if (std::isinf(v)) {
        out.append(v > 0 ? "+Inf" : "-Inf");
        return;
    }
    // Prometheus 建议高精度: %.17g 可精确往返 double
    // 但实际使用中 %.6g 更可读，且我们的指标不需要 17 位精度
    char tmp[32];
    int len;
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        len = std::snprintf(tmp, sizeof(tmp), "%.0f", v);
    } else {
        len = std::snprintf(tmp, sizeof(tmp), "%.6g", v);
    }
    out.append(tmp, static_cast<size_t>(len));
}

// ════════════════════════════════════════════
// 各类型写入
// ════════════════════════════════════════════

void PrometheusExporter::write_counter(std::string& out, const Counter& c) {
    const auto& name = c.meta().name;

    // # HELP
    out.append("# HELP ");
    out.append(name);
    out.push_back(' ');
    escape_help(out, c.meta().help);
    out.push_back('\n');

    // # TYPE
    out.append("# TYPE ");
    out.append(name);
    out.append(" counter\n");

    // 数据行
    out.append(name);
    out.push_back(' ');
    format_value(out, c.value());
    out.push_back('\n');

    // 空行分隔
    out.push_back('\n');
}

void PrometheusExporter::write_gauge(std::string& out, const Gauge& g) {
    const auto& name = g.meta().name;

    out.append("# HELP ");
    out.append(name);
    out.push_back(' ');
    escape_help(out, g.meta().help);
    out.push_back('\n');

    out.append("# TYPE ");
    out.append(name);
    out.append(" gauge\n");

    out.append(name);
    out.push_back(' ');
    format_value(out, g.value());
    out.push_back('\n');
    out.push_back('\n');
}

void PrometheusExporter::write_bridge_gauge(std::string& out,
                                             const MetricsRegistry::BridgeGauge& bg) {
    const auto& name = bg.meta.name;

    double val = 0.0;
    try {
        val = bg.callback();
    } catch (...) {
        val = 0.0;
    }

    out.append("# HELP ");
    out.append(name);
    out.push_back(' ');
    escape_help(out, bg.meta.help);
    out.push_back('\n');

    out.append("# TYPE ");
    out.append(name);
    out.append(" gauge\n");

    out.append(name);
    out.push_back(' ');
    format_value(out, val);
    out.push_back('\n');
    out.push_back('\n');
}

void PrometheusExporter::write_histogram(std::string& out, const Histogram& h) {
    const auto& name = h.meta().name;
    auto snap = h.snapshot();

    out.append("# HELP ");
    out.append(name);
    out.push_back(' ');
    escape_help(out, h.meta().help);
    out.push_back('\n');

    out.append("# TYPE ");
    out.append(name);
    out.append(" histogram\n");

    // 桶 (累积)
    for (size_t i = 0; i < snap.boundaries.size(); ++i) {
        out.append(name);
        out.append("_bucket{le=\"");
        char le_buf[32];
        std::snprintf(le_buf, sizeof(le_buf), "%.6g", snap.boundaries[i]);
        out.append(le_buf);
        out.append("\"} ");
        format_value(out, snap.bucket_counts[i]);
        out.push_back('\n');
    }

    // +Inf 桶 (必须)
    out.append(name);
    out.append("_bucket{le=\"+Inf\"} ");
    format_value(out, snap.bucket_counts.back());
    out.push_back('\n');

    // _sum
    out.append(name);
    out.append("_sum ");
    format_value(out, snap.sum);
    out.push_back('\n');

    // _count
    out.append(name);
    out.append("_count ");
    format_value(out, snap.count);
    out.push_back('\n');

    out.push_back('\n');
}

// ════════════════════════════════════════════
// 主序列化函数
// ════════════════════════════════════════════

std::string PrometheusExporter::serialize(const MetricsRegistry& registry) const {
    std::string out;
    out.reserve(registry.size() * 128); // 动态预估：平均每个指标块约 128 字节

    for (const auto& entry : registry.entries()) {
        switch (entry.kind) {
            case MetricsRegistry::MetricKind::COUNTER: {
                auto* c = registry.find_counter(entry.name);
                if (c) write_counter(out, *c);
                break;
            }
            case MetricsRegistry::MetricKind::GAUGE: {
                auto* g = registry.find_gauge(entry.name);
                if (g) write_gauge(out, *g);
                break;
            }
            case MetricsRegistry::MetricKind::BRIDGE_GAUGE: {
                auto* bg = registry.find_bridge(entry.name);
                if (bg) write_bridge_gauge(out, *bg);
                break;
            }
            case MetricsRegistry::MetricKind::HISTOGRAM: {
                auto* h = registry.find_histogram(entry.name);
                if (h) write_histogram(out, *h);
                break;
            }
        }
    }

    return out;
}