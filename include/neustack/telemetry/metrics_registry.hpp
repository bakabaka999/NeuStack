#ifndef NEUSTACK_TELEMETRY_METRICS_REGISTRY_HPP
#define NEUSTACK_TELEMETRY_METRICS_REGISTRY_HPP

#include "neustack/telemetry/metric_types.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <variant>
#include <functional>
#include <vector>
#include <stdexcept>

namespace neustack::telemetry {

/**
 * 指标注册中心
 *
 * 所有指标在启动时注册（写），运行时只读取（读）。
 * 注册发生在单线程初始化阶段，读取可并发。
 *
 * 生命周期:
 *   1. 构造阶段: 调用 counter()/gauge()/histogram()/bridge_gauge() 注册指标
 *   2. 运行阶段: 通过返回的引用更新指标值，通过 entries()/find_*() 读取
 *   3. 销毁阶段: Registry 析构时自动释放所有指标
 *
 * 注意: 注册返回的引用在 Registry 存活期间保持有效。
 *       不要在 Registry 析构后使用这些引用。
 *
 * 使用方式:
 *   auto& reg = MetricsRegistry::instance();
 *
 *   // 启动时注册
 *   auto& pkts_rx = reg.counter("neustack_packets_rx_total", "Total packets received");
 *   auto& rtt = reg.histogram("neustack_tcp_rtt_us", "TCP RTT distribution",
 *                             {100, 500, 1000, 5000, 10000, 50000});
 *
 *   // 数据面使用
 *   pkts_rx.increment();
 *   rtt.observe(350);  // 350us
 *
 *   // 查询面导出
 *   for (const auto& entry : reg.entries()) { ... }
 */
class MetricsRegistry {
public:
    /**
     * 全局单例
     *
     * 使用 Meyers' Singleton 模式，线程安全的首次初始化。
     * 单例在程序退出时自动销毁。
     */
    static MetricsRegistry& instance() {
        // 利用静态变量的 Magic Static 实现原子性初始化
        // 以及，可以保证延迟初始化与实例唯一性
        static MetricsRegistry reg;
        return reg;
    }

    // 允许构造独立实例（用于测试）
    MetricsRegistry() = default;

    // 禁止拷贝和移动
    MetricsRegistry(const MetricsRegistry &) = delete;
    MetricsRegistry &operator=(const MetricsRegistry &) = delete;
    MetricsRegistry(MetricsRegistry &&) = delete;
    MetricsRegistry &operator=(MetricsRegistry &&) = delete;

    // 枚举类型与实体定义
    enum class MetricKind { COUNTER, GAUGE, HISTOGRAM, BRIDGE_GAUGE };

    struct MetricEntry {
        MetricKind kind;
        std::string name;
    };

    // ─── 注册接口 (启动阶段调用) ───

    /**
     * 注册一个 Counter 指标
     *
     * @param name 指标名（建议遵循 Prometheus 命名规范）
     * @param help 帮助文本
     * @param unit 单位（可选）
     * @return Counter 引用，在 Registry 存活期间有效
     *
     * 线程安全: 是（内部加锁）
     * 注意: 重复注册同名 Counter 会覆盖旧值
     */
    Counter& counter(const std::string& name, const std::string& help,
                     const std::string& unit = "") {
        auto ptr = std::make_unique<Counter>(MetricMeta{name, help, unit});
        auto& ref = *ptr;
        std::lock_guard lock(_mutex);
        bool is_new = (_counters.find(name) == _counters.end());
        _counters[name] = std::move(ptr);
        if (is_new) _order.push_back({MetricKind::COUNTER, name});
        return ref;
    }

    /**
     * 注册一个 Gauge 指标
     *
     * @param name 指标名
     * @param help 帮助文本
     * @param unit 单位（可选）
     * @return Gauge 引用
     */
    Gauge& gauge(const std::string& name, const std::string& help,
                 const std::string& unit = "") {
        auto ptr = std::make_unique<Gauge>(MetricMeta{name, help, unit});
        auto& ref = *ptr;
        std::lock_guard lock(_mutex);
        bool is_new = (_gauges.find(name) == _gauges.end());
        _gauges[name] = std::move(ptr);
        if (is_new) _order.push_back({MetricKind::GAUGE, name});
        return ref;
    }

    /**
     * 注册一个 Histogram 指标
     *
     * @param name 指标名
     * @param help 帮助文本
     * @param boundaries 桶上界数组，必须递增
     * @param unit 单位（可选）
     * @return Histogram 引用
     */
    Histogram& histogram(const std::string& name, const std::string& help,
                         std::vector<double> boundaries,
                         const std::string& unit = "") {
        auto ptr = std::make_unique<Histogram>(
            MetricMeta{name, help, unit}, std::move(boundaries));
        auto& ref = *ptr;
        std::lock_guard lock(_mutex);
        bool is_new = (_histograms.find(name) == _histograms.end());
        _histograms[name] = std::move(ptr);
        if (is_new) _order.push_back({MetricKind::HISTOGRAM, name});
        return ref;
    }

    // ─── 桥接接口: 注册外部数据源 (lazy 读取) ───

    /**
     * 注册一个 "虚拟" Gauge，值由回调函数提供
     * 用于桥接 GlobalMetrics / SecurityMetrics / NetworkAgent 等现有指标
     *
     * 回调语义:
     *   - 回调在导出/查询时被调用（查询线程）
     *   - 回调应当是轻量的（<100ns），不应阻塞
     *   - 回调中引用的对象必须在 Registry 存活期间有效
     *
     * 示例:
     *   reg.bridge_gauge("neustack_active_connections", "Active TCP connections",
     *       [&metrics]() { return metrics.active_connections.load(); });
     */
    using GaugeCallback = std::function<double()>;

    void bridge_gauge(const std::string& name, const std::string& help,
                      GaugeCallback cb, const std::string& unit = "") {
        std::lock_guard lock(_mutex);
        bool is_new = (_bridges.find(name) == _bridges.end());
        _bridges[name] = {MetricMeta{name, help, unit}, std::move(cb)};
        if (is_new) _order.push_back({MetricKind::BRIDGE_GAUGE, name});
    }

    // ─── 遍历接口 (导出器使用) ───

    /**
     * 按注册顺序返回所有指标条目
     *
     * 线程安全: 注册完成后可并发读取（只要不再注册新指标）
     */
    const std::vector<MetricEntry>& entries() const { return _order; }

    /**
     * 获取已注册的指标总数
     */
    size_t size() const {
        return _order.size();
    }

    // 按名称查找（返回 nullptr 表示未找到）
    Counter* find_counter(const std::string& name) const {
        auto it = _counters.find(name);
        return it != _counters.end() ? it->second.get() : nullptr;
    }

    Gauge* find_gauge(const std::string& name) const {
        auto it = _gauges.find(name);
        return it != _gauges.end() ? it->second.get() : nullptr;
    }

    Histogram* find_histogram(const std::string& name) const {
        auto it = _histograms.find(name);
        return it != _histograms.end() ? it->second.get() : nullptr;
    }

    struct BridgeGauge {
        MetricMeta meta;
        GaugeCallback callback;
    };

    const BridgeGauge* find_bridge(const std::string& name) const {
        auto it = _bridges.find(name);
        return it != _bridges.end() ? &it->second : nullptr;
    }

    // ─── 管理接口 ───

    /**
     * 清除所有已注册的指标
     *
     * 主要用于测试。生产环境中不应调用。
     * 调用后，之前返回的所有引用均失效。
     */
    void clear() {
        std::lock_guard lock(_mutex);
        _counters.clear();
        _gauges.clear();
        _histograms.clear();
        _bridges.clear();
        _order.clear();
    }

private:
    mutable std::mutex _mutex; // 只在注册/查找时加锁
    std::unordered_map<std::string, std::unique_ptr<Counter>> _counters;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> _gauges;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> _histograms;
    std::unordered_map<std::string, BridgeGauge> _bridges;
    std::vector<MetricEntry> _order; // 保持注册顺序
};

/**
 * 注册所有内置指标
 *
 * 在 NeuStack::create() 中调用一次。
 *
 * 设计决策:
 *   - 使用 bridge_gauge 而非直接注册 Counter，是因为 GlobalMetrics
 *     中的 atomic 已经存在，不需要额外的 Counter 实例
 *   - bridge_gauge 的回调捕获 GlobalMetrics 引用，每次导出时调用
 *   - 这种方式零开销：数据面完全不受影响
 */
void register_builtin_metrics(GlobalMetrics &gm, const SecurityMetrics *sm);

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_METRICS_REGISTRY_HPP