#ifndef NEUSTACK_TELEMETRY_EXPORTER_HPP
#define NEUSTACK_TELEMETRY_EXPORTER_HPP

#include <string>

namespace neustack::telemetry {
    
class MetricsRegistry; // 前向声明

/**
 * 导出器基类（可选继承）
 *
 * v1.3 的 JsonExporter / PrometheusExporter 使用静态方法，
 * 不强制继承此基类。此接口为 v2.0 动态注册导出器预留。
 */
class Exporter {
public:
    virtual ~Exporter() = default;

    /** 返回格式名称，如 "json", "prometheus", "otlp" */
    virtual std::string format_name() const = 0;

    /** 返回 HTTP Content-Type */
    virtual std::string content_type() const = 0;

    /** 序列化所有指标 */
    virtual std::string serialize(const MetricsRegistry &registry) const = 0;
};

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_EXPORTER_HPP