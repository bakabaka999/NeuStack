#include "neustack/telemetry/json_exporter.hpp"
#include "neustack/common/json_builder.hpp"
#include <cstdio>
#include <chrono>
#include <ctime>

using namespace neustack::telemetry;
using neustack::JsonBuilder;

// ════════════════════════════════════════════
// 工具
// ════════════════════════════════════════════

namespace {

std::string iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

} // anonymous namespace

// ════════════════════════════════════════════
// Histogram percentile
// ════════════════════════════════════════════

double JsonExporter::estimate_percentile(const Histogram::Snapshot& snap, double q) {
    if (snap.count == 0) return 0.0;

    double rank = q * static_cast<double>(snap.count);

    size_t i = 0;
    for (; i < snap.bucket_counts.size(); ++i) {
        if (static_cast<double>(snap.bucket_counts[i]) >= rank) break;
    }

    double lower = 0.0;
    double prev_count = 0.0;
    if (i > 0) {
        lower = snap.boundaries[i - 1];
        prev_count = static_cast<double>(snap.bucket_counts[i - 1]);
    }

    double upper = 0.0;
    if (i < snap.boundaries.size())
        upper = snap.boundaries[i];
    else
        upper = snap.boundaries.empty() ? 0.0 : snap.boundaries.back() * 2.0;

    double curr_bucket_count = static_cast<double>(snap.bucket_counts[i]) - prev_count;
    if (curr_bucket_count <= 0) return lower;

    double fraction = (rank - prev_count) / curr_bucket_count;
    return lower + fraction * (upper - lower);
}

// ════════════════════════════════════════════
// 各类型序列化
// ════════════════════════════════════════════

void JsonExporter::serialize_counter(JsonBuilder& b, const Counter& c) {
    b.begin_object();
    b.key("help");  b.write_string(c.meta().help);  b.comma();
    b.key("unit");  b.write_string(c.meta().unit);  b.comma();
    b.key("value"); b.write_uint64(c.value());
    b.end_object();
}

void JsonExporter::serialize_gauge(JsonBuilder& b, const Gauge& g) {
    b.begin_object();
    b.key("help");  b.write_string(g.meta().help);  b.comma();
    b.key("unit");  b.write_string(g.meta().unit);  b.comma();
    b.key("value"); b.write_double(g.value());
    b.end_object();
}

void JsonExporter::serialize_bridge(JsonBuilder& b, const MetricsRegistry::BridgeGauge& bg) {
    double val = 0.0;
    try { val = bg.callback(); } catch (...) {}

    b.begin_object();
    b.key("help");  b.write_string(bg.meta.help);  b.comma();
    b.key("unit");  b.write_string(bg.meta.unit);  b.comma();
    b.key("value"); b.write_double(val);
    b.end_object();
}

void JsonExporter::serialize_histogram(JsonBuilder& b, const Histogram& h) {
    auto snap = h.snapshot();

    b.begin_object();
    b.key("help");  b.write_string(h.meta().help);  b.comma();
    b.key("unit");  b.write_string(h.meta().unit);  b.comma();
    b.key("count"); b.write_uint64(snap.count);      b.comma();
    b.key("sum");   b.write_double(snap.sum);        b.comma();

    b.key("buckets");
    b.begin_object();
    for (size_t i = 0; i < snap.boundaries.size(); ++i) {
        if (i > 0) b.comma();
        char key_buf[32];
        std::snprintf(key_buf, sizeof(key_buf), "%.6g", snap.boundaries[i]);
        b.key(key_buf);
        b.write_uint64(snap.bucket_counts[i]);
    }
    if (!snap.boundaries.empty()) b.comma();
    b.key("+Inf"); b.write_uint64(snap.bucket_counts.back());
    b.end_object();
    b.comma();

    b.key("percentiles");
    b.begin_object();
    b.key("p50"); b.write_double(estimate_percentile(snap, 0.50)); b.comma();
    b.key("p90"); b.write_double(estimate_percentile(snap, 0.90)); b.comma();
    b.key("p99"); b.write_double(estimate_percentile(snap, 0.99));
    b.end_object();

    b.end_object();
}

// ════════════════════════════════════════════
// 主序列化
// ════════════════════════════════════════════

// 复用逻辑：把 entries 遍历和分组写入抽成一个内部函数
static void write_metrics_object(JsonBuilder& b,
                                 const MetricsRegistry& registry,
                                 const std::string* prefix)
{
    auto matches = [&](const std::string& name) {
        if (!prefix) return true;
        return name.compare(0, prefix->size(), *prefix) == 0;
    };

    b.begin_object();
    int section_idx = 0;

    // ─── Counters ───
    bool has = false;
    for (const auto& e : registry.entries()) {
        if (e.kind != MetricsRegistry::MetricKind::COUNTER) continue;
        if (!matches(e.name)) continue;
        if (!has) {
            if (section_idx++ > 0) b.comma();
            b.key("counters"); b.begin_object(); has = true;
        } else { b.comma(); }
        auto* c = registry.find_counter(e.name);
        if (c) { b.key(e.name); JsonExporter::serialize_counter(b, *c); }  // 借用 static
    }
    if (has) b.end_object();

    // ─── Gauges (native + bridge) ───
    has = false;
    for (const auto& e : registry.entries()) {
        bool is_gauge = (e.kind == MetricsRegistry::MetricKind::GAUGE ||
                         e.kind == MetricsRegistry::MetricKind::BRIDGE_GAUGE);
        if (!is_gauge) continue;
        if (!matches(e.name)) continue;
        if (!has) {
            if (section_idx++ > 0) b.comma();
            b.key("gauges"); b.begin_object(); has = true;
        } else { b.comma(); }
        if (e.kind == MetricsRegistry::MetricKind::GAUGE) {
            auto* g = registry.find_gauge(e.name);
            if (g) { b.key(e.name); JsonExporter::serialize_gauge(b, *g); }
        } else {
            auto* bg = registry.find_bridge(e.name);
            if (bg) { b.key(e.name); JsonExporter::serialize_bridge(b, *bg); }
        }
    }
    if (has) b.end_object();

    // ─── Histograms ───
    has = false;
    for (const auto& e : registry.entries()) {
        if (e.kind != MetricsRegistry::MetricKind::HISTOGRAM) continue;
        if (!matches(e.name)) continue;
        if (!has) {
            if (section_idx++ > 0) b.comma();
            b.key("histograms"); b.begin_object(); has = true;
        } else { b.comma(); }
        auto* h = registry.find_histogram(e.name);
        if (h) { b.key(e.name); JsonExporter::serialize_histogram(b, *h); }
    }
    if (has) b.end_object();

    b.end_object();
}

std::string JsonExporter::serialize(const MetricsRegistry& registry) const {
    JsonBuilder b(_pretty, 4096);
    b.begin_object();
    b.key("timestamp"); b.write_string(iso8601_now()); b.comma();
    b.key("metrics");   write_metrics_object(b, registry, nullptr);
    b.end_object();
    return std::move(b.buf);
}

std::string JsonExporter::serialize_filtered(const MetricsRegistry& registry,
                                              const std::string& prefix) const {
    JsonBuilder b(_pretty, 4096);
    b.begin_object();
    b.key("timestamp"); b.write_string(iso8601_now()); b.comma();
    b.key("filter");    b.write_string(prefix);        b.comma();
    b.key("metrics");   write_metrics_object(b, registry, &prefix);
    b.end_object();
    return std::move(b.buf);
}
