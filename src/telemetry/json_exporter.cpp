#include "neustack/telemetry/json_exporter.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <ctime>

using namespace neustack::telemetry;

// ════════════════════════════════════════════
// JsonBuilder 实现
// ════════════════════════════════════════════

void JsonExporter::JsonBuilder::indent() {
    if (!pretty)
        return;
    for (int i = 0; i < depth; i++) {
        buf.append("  ");
    }
}

void JsonExporter::JsonBuilder::newline() {
    if (pretty)
        buf.push_back('\n');
}

void JsonExporter::JsonBuilder::write_string(std::string_view s) {
    buf.push_back('"');
    escape_string(buf, s);
    buf.push_back('"');
}

void JsonExporter::JsonBuilder::write_uint64(uint64_t v) {
    format_uint64(buf, v);
}

void JsonExporter::JsonBuilder::write_double(double v) {
    format_double(buf, v);
}

void JsonExporter::JsonBuilder::write_raw(std::string_view s) {
    buf.append(s);
}

void JsonExporter::JsonBuilder::begin_object() {
    buf.push_back('{');
    newline();
    ++depth;
}

void JsonExporter::JsonBuilder::end_object() {
    --depth;
    newline();
    indent();
    buf.push_back('}');
}

void JsonExporter::JsonBuilder::begin_array() {
    buf.push_back('[');
    newline();
    ++depth;
}

void JsonExporter::JsonBuilder::end_array() {
    --depth;
    newline();
    indent();
    buf.push_back(']');
}

void JsonExporter::JsonBuilder::key(std::string_view k) {
    indent();
    write_string(k);
    buf.push_back(':');
    if (pretty) buf.push_back(' ');
}

void JsonExporter::JsonBuilder::comma() {
    buf.push_back(',');
    newline();
}

// ════════════════════════════════════════════
// 字符串转义
// ════════════════════════════════════════════

void JsonExporter::escape_string(std::string &out, std::string_view in) {
    for (char c : in) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 控制字符: \u00XX
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(hex);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
}

// ════════════════════════════════════════════
// 数值格式化
// ════════════════════════════════════════════

void JsonExporter::format_uint64(std::string& out, uint64_t v) {
    // 避免 std::to_string 的临时分配：直接 snprintf 到 buffer
    char tmp[24];
    int len = std::snprintf(tmp, sizeof(tmp), "%llu",
                            static_cast<unsigned long long>(v));
    out.append(tmp, static_cast<size_t>(len));
}

void JsonExporter::format_double(std::string& out, double v) {
    // 特殊值处理
    if (std::isnan(v)) {
        out.append("null");  // JSON 没有 NaN，用 null
        return;
    }
    if (std::isinf(v)) {
        out.append(v > 0 ? "1e308" : "-1e308");  // JSON 没有 Inf，用大数近似
        return;
    }

    // %.6g: 最多 6 位有效数字，自动选择定点或科学计数法
    // 整数值不带小数点: 42.0 → "42" (通过检查是否为整数)
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        // 可以精确表示为整数
        char tmp[24];
        int len = std::snprintf(tmp, sizeof(tmp), "%.0f", v);
        out.append(tmp, static_cast<size_t>(len));
    } else {
        char tmp[32];
        int len = std::snprintf(tmp, sizeof(tmp), "%.6g", v);
        out.append(tmp, static_cast<size_t>(len));
    }
}

/**
 * 基于线性插值算法估算百分位数 (P50/P90/P99)
 * 算法参考自 Prometheus histogram_quantile 逻辑
 */
double JsonExporter::estimate_percentile(const Histogram::Snapshot& snap, double q) {
    // 边界情况 1：没有任何样本
    if (snap.count == 0) return 0.0;

    // 1. 确定目标排名 (Rank)
    // 例如 count=100, q=0.99，目标就是第 99 个样本
    double rank = q * static_cast<double>(snap.count);

    // 2. 找到 rank 落入的桶 i
    // snap.bucket_counts 是累积计数，例如 [10, 50, 90, 100]
    size_t i = 0;
    for (; i < snap.bucket_counts.size(); ++i) {
        if (static_cast<double>(snap.bucket_counts[i]) >= rank) {
            break;
        }
    }

    // 3. 确定该桶的物理边界 (Lower/Upper Bound)
    double lower = 0.0;
    double prev_count = 0.0;

    if (i > 0) {
        // 如果不是第一个桶，下界就是前一个桶的上界
        lower = snap.boundaries[i - 1];
        prev_count = static_cast<double>(snap.bucket_counts[i - 1]);
    }

    double upper = 0.0;
    if (i < snap.boundaries.size()) {
        // 普通桶的上界
        upper = snap.boundaries[i];
    } else {
        // 边界情况 3：所有样本都在最后一个桶 (+Inf 桶)
        // 假设上界是最后已知边界的 2 倍
        upper = snap.boundaries.empty() ? 0.0 : snap.boundaries.back() * 2.0;
    }

    // 4. 线性插值计算
    double curr_bucket_count = static_cast<double>(snap.bucket_counts[i]) - prev_count;
    if (curr_bucket_count <= 0) return lower;

    // 计算 rank 在当前桶内的偏移比例
    double fraction = (rank - prev_count) / curr_bucket_count;
    
    // 最终值 = 下界 + (比例 * 桶宽度)
    return lower + fraction * (upper - lower);
}

namespace {

std::string iso8601_now()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

} // anonymous namespace

// ════════════════════════════════════════════
// 各类型序列化
// ════════════════════════════════════════════

void JsonExporter::serialize_counter(JsonBuilder& b, const Counter& c) {
    b.begin_object();
    b.key("help");
    b.write_string(c.meta().help);
    b.comma();
    b.key("unit");
    b.write_string(c.meta().unit);
    b.comma();
    b.key("value");
    b.write_uint64(c.value());
    b.end_object();
}

void JsonExporter::serialize_gauge(JsonBuilder& b, const Gauge& g) {
    b.begin_object();
    b.key("help");
    b.write_string(g.meta().help);
    b.comma();
    b.key("unit");
    b.write_string(g.meta().unit);
    b.comma();
    b.key("value");
    b.write_double(g.value());
    b.end_object();
}

void JsonExporter::serialize_bridge(JsonBuilder& b,
                                     const MetricsRegistry::BridgeGauge& bg) {
    double val = 0.0;
    try {
        val = bg.callback();
    } catch (...) {
        val = 0.0;
    }

    b.begin_object();
    b.key("help");
    b.write_string(bg.meta.help);
    b.comma();
    b.key("unit");
    b.write_string(bg.meta.unit);
    b.comma();
    b.key("value");
    b.write_double(val);
    b.end_object();
}

void JsonExporter::serialize_histogram(JsonBuilder& b, const Histogram& h) {
    auto snap = h.snapshot();

    b.begin_object();
    b.key("help");
    b.write_string(h.meta().help);
    b.comma();
    b.key("unit");
    b.write_string(h.meta().unit);
    b.comma();
    b.key("count");
    b.write_uint64(snap.count);
    b.comma();
    b.key("sum");
    b.write_double(snap.sum);
    b.comma();

    // 桶 (累积)
    b.key("buckets");
    b.begin_object();
    for (size_t i = 0; i < snap.boundaries.size(); ++i) {
        if (i > 0) b.comma();
        // 桶上界作为键
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "%.6g", snap.boundaries[i]);
        b.key(key_buf);
        b.write_uint64(snap.bucket_counts[i]);
    }
    // +Inf 桶
    if (!snap.boundaries.empty()) b.comma();
    b.key("+Inf");
    b.write_uint64(snap.bucket_counts.back());
    b.end_object();
    b.comma();

    // Percentile 估算
    b.key("percentiles");
    b.begin_object();
    b.key("p50");
    b.write_double(estimate_percentile(snap, 0.50));
    b.comma();
    b.key("p90");
    b.write_double(estimate_percentile(snap, 0.90));
    b.comma();
    b.key("p99");
    b.write_double(estimate_percentile(snap, 0.99));
    b.end_object();

    b.end_object();
}

// ════════════════════════════════════════════
// 主序列化函数
// ════════════════════════════════════════════

std::string JsonExporter::serialize(const MetricsRegistry& registry) const {
    // 预估 buffer: 每个指标约 80 字节 (compact)，50 个指标 ≈ 4KB
    JsonBuilder b(_pretty, 4096);

    b.begin_object();

    // timestamp
    b.key("timestamp");
    b.write_string(iso8601_now());
    b.comma();

    // metrics 外层
    b.key("metrics");
    b.begin_object();

    // 用 section_idx 追踪是否需要前置逗号
    int section_idx = 0;

    // ─── Counters ───
    bool has_counters = false;
    for (const auto& entry : registry.entries()) {
        if (entry.kind == MetricsRegistry::MetricKind::COUNTER) {
            if (!has_counters) {
                if (section_idx > 0) b.comma();
                b.key("counters");
                b.begin_object();
                has_counters = true;
            } else {
                b.comma();
            }
            auto* c = registry.find_counter(entry.name);
            if (c) {
                b.key(entry.name);
                serialize_counter(b, *c);
            }
        }
    }
    if (has_counters) {
        b.end_object();
        ++section_idx;
    }

    // ─── Gauges (native + bridge) ───
    bool has_gauges = false;
    for (const auto& entry : registry.entries()) {
        bool is_gauge = (entry.kind == MetricsRegistry::MetricKind::GAUGE ||
                         entry.kind == MetricsRegistry::MetricKind::BRIDGE_GAUGE);
        if (!is_gauge) continue;

        if (!has_gauges) {
            if (section_idx > 0) b.comma();
            b.key("gauges");
            b.begin_object();
            has_gauges = true;
        } else {
            b.comma();
        }

        if (entry.kind == MetricsRegistry::MetricKind::GAUGE) {
            auto* g = registry.find_gauge(entry.name);
            if (g) {
                b.key(entry.name);
                serialize_gauge(b, *g);
            }
        } else {
            auto* bg = registry.find_bridge(entry.name);
            if (bg) {
                b.key(entry.name);
                serialize_bridge(b, *bg);
            }
        }
    }
    if (has_gauges) {
        b.end_object();
        ++section_idx;
    }

    // ─── Histograms ───
    bool has_histograms = false;
    for (const auto& entry : registry.entries()) {
        if (entry.kind != MetricsRegistry::MetricKind::HISTOGRAM) continue;

        if (!has_histograms) {
            if (section_idx > 0) b.comma();
            b.key("histograms");
            b.begin_object();
            has_histograms = true;
        } else {
            b.comma();
        }

        auto* h = registry.find_histogram(entry.name);
        if (h) {
            b.key(entry.name);
            serialize_histogram(b, *h);
        }
    }
    if (has_histograms) {
        b.end_object();
    }

    b.end_object();  // metrics
    b.end_object();  // root

    return std::move(b.buf);
}

std::string JsonExporter::serialize_filtered(const MetricsRegistry& registry,
                                              const std::string& prefix) const {
    JsonBuilder b(_pretty, 4096);

    b.begin_object();

    b.key("timestamp");
    b.write_string(iso8601_now());
    b.comma();

    b.key("filter");
    b.write_string(prefix);
    b.comma();

    b.key("metrics");
    b.begin_object();

    int section_idx = 0;

    // ─── Counters ───
    bool has_counters = false;
    for (const auto& entry : registry.entries()) {
        if (entry.kind != MetricsRegistry::MetricKind::COUNTER) continue;
        if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;

        if (!has_counters) {
            if (section_idx > 0) b.comma();
            b.key("counters");
            b.begin_object();
            has_counters = true;
        } else {
            b.comma();
        }
        auto* c = registry.find_counter(entry.name);
        if (c) {
            b.key(entry.name);
            serialize_counter(b, *c);
        }
    }
    if (has_counters) {
        b.end_object();
        ++section_idx;
    }

    // ─── Gauges (native + bridge) ───
    bool has_gauges = false;
    for (const auto& entry : registry.entries()) {
        bool is_gauge = (entry.kind == MetricsRegistry::MetricKind::GAUGE ||
                         entry.kind == MetricsRegistry::MetricKind::BRIDGE_GAUGE);
        if (!is_gauge) continue;
        if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;

        if (!has_gauges) {
            if (section_idx > 0) b.comma();
            b.key("gauges");
            b.begin_object();
            has_gauges = true;
        } else {
            b.comma();
        }

        if (entry.kind == MetricsRegistry::MetricKind::GAUGE) {
            auto* g = registry.find_gauge(entry.name);
            if (g) {
                b.key(entry.name);
                serialize_gauge(b, *g);
            }
        } else {
            auto* bg = registry.find_bridge(entry.name);
            if (bg) {
                b.key(entry.name);
                serialize_bridge(b, *bg);
            }
        }
    }
    if (has_gauges) {
        b.end_object();
        ++section_idx;
    }

    // ─── Histograms ───
    bool has_histograms = false;
    for (const auto& entry : registry.entries()) {
        if (entry.kind != MetricsRegistry::MetricKind::HISTOGRAM) continue;
        if (entry.name.compare(0, prefix.size(), prefix) != 0) continue;

        if (!has_histograms) {
            if (section_idx > 0) b.comma();
            b.key("histograms");
            b.begin_object();
            has_histograms = true;
        } else {
            b.comma();
        }

        auto* h = registry.find_histogram(entry.name);
        if (h) {
            b.key(entry.name);
            serialize_histogram(b, *h);
        }
    }
    if (has_histograms) {
        b.end_object();
    }

    b.end_object();  // metrics
    b.end_object();  // root

    return std::move(b.buf);
}