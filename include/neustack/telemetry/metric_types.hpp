#ifndef NEUSTACK_TELEMETRY_METRIC_TYPES_HPP
#define NEUSTACK_TELEMETRY_METRIC_TYPES_HPP

#include <cstdint>
#include <atomic>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <limits>

namespace neustack::telemetry {
    
// ─── 指标元信息 ───

struct MetricMeta {
    std::string name;         // e.g. "neustack_tcp_packets_rx_total"
    std::string help;         // e.g. "Total TCP packets received"
    std::string unit;         // e.g. "bytes", "packets", "" (无单位)

    // 标签 (Prometheus label), 用于区分实例
    // e.g. {"protocol": "tcp"}, {"direction": "rx"}
    // 简化: 暂时将标签嵌入 name 中，不做动态标签
};

// ─── Counter: 只增不减的计数器 ───

class Counter {
public:
    explicit Counter(MetricMeta meta) : _meta(std::move(meta)) {}

    // 禁止拷贝
    Counter(const Counter &) = delete;
    Counter &operator=(const Counter &) = delete;

    /**
     * 递增计数器
     *
     * 线程安全: 是（atomic fetch_add, relaxed ordering）
     * 热路径开销: ~2-5 ns（无竞争时等价于普通加法）
     *
     * @param delta 增量，必须 >= 0
     */
    void increment(uint64_t delta = 1) {
        _value.fetch_add(delta, std::memory_order_relaxed);
    }

    /**
     * 读取当前值
     *
     * 线程安全: 是（atomic load, relaxed ordering）
     * 注意: 返回的是某一时刻的近似值，不保证全局一致性
     */
    uint64_t value() const {
        return _value.load(std::memory_order_relaxed);
    }

    const MetricMeta& meta() const { return _meta; }

private:
    MetricMeta _meta;
    alignas(64) std::atomic<uint64_t> _value{0}; // 独占缓冲行
};

// ─── Gauge: 可增可减的瞬时值 ───

class Gauge {
public:
    explicit Gauge(MetricMeta meta) : _meta(std::move(meta)) {}

    Gauge(const Gauge &) = delete;
    Gauge &operator=(const Gauge &) = delete;

    /**
     * 设置 Gauge 的值
     *
     * 线程安全: 是（通过 atomic uint64_t 的 bit-cast 实现）
     * 实现说明: C++ 标准不保证 atomic<double>，
     *          所以用 uint64_t bit-cast 替代。
     */
    void set(double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(bits));
        _bits.store(bits, std::memory_order_relaxed);
    }

    /**
     * 递增 Gauge
     *
     * 线程安全: 弱（read-modify-write 不是原子的）
     * 如果有多个写者，建议使用 set() 替代
     */
    void increment(double delta = 1.0) {
        set(value() + delta);
    }

    /**
     * 递减 Gauge
     *
     * 线程安全: 同 increment()，弱线程安全
     */
    void decrement(double delta = 1.0) {
        set(value() - delta);
    }

    double value() const {
        uint64_t bits = _bits.load(std::memory_order_relaxed);
        double val;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }

    const MetricMeta &meta() const { return _meta; }

private:
    MetricMeta _meta;
    alignas(64) std::atomic<uint64_t> _bits{0}; // bit-punned double，独占缓存行
};

// ─── Histogram: 分布统计 (固定桶) ───

/**
 * 适用于 RTT 分布、包大小分布等
 *
 * 桶边界在构造时固定，运行时只做线性查找 + atomic increment。
 *
 * 线程安全: 所有操作均线程安全
 *   - observe(): 多线程并发安全（每个桶独立 atomic）
 *   - snapshot(): 可与 observe() 并发调用（返回近似快照）
 *
 * Prometheus 导出格式:
 *   neustack_tcp_rtt_us_bucket{le="100"} 42
 *   neustack_tcp_rtt_us_bucket{le="500"} 128
 *   ...
 *   neustack_tcp_rtt_us_bucket{le="+Inf"} 200
 *   neustack_tcp_rtt_us_sum 25000
 *   neustack_tcp_rtt_us_count 200
 */
class Histogram {
public:
    static constexpr size_t MAX_BUCKETS = 32;

    /**
     * @param meta 指标元信息
     * @param boundaries 桶上界数组，必须递增。最多 MAX_BUCKETS-1 个。
     *                   自动追加 +Inf 桶。
     *
     * 例: boundaries = {100, 500, 1000, 5000, 10000}
     *     实际桶: [0,100], (100,500], ..., (10000,+Inf)
     */
    Histogram(MetricMeta meta, std::vector<double> boundaries)
        : _meta(std::move(meta))
        , _boundaries(std::move(boundaries))
        , _bucket_count(_boundaries.size() + 1)  // +1 for +Inf
    {
        for (size_t i = 0; i < MAX_BUCKETS; ++i) {
            _buckets[i].store(0, std::memory_order_relaxed);
        }
    }

    Histogram(const Histogram &) = delete;
    Histogram &operator=(const Histogram &) = delete;

    /**
     * 记录一个观测值
     *
     * 线程安全: 是（每个桶独立 atomic increment + CAS loop for sum）
     * 热路径: O(N) 线性查找桶 + 1 次 atomic increment + sum/count 更新
     * 实际开销: 11 个桶时约 20-30 ns
     */
    void observe(double value) {
        // 找到对应的桶
        size_t idx = _bucket_count - 1;  // 默认放 +Inf 桶
        for (size_t i = 0; i < _boundaries.size(); ++i) {
            if (value <= _boundaries[i]) {
                idx = i;
                break;
            }
        }
        _buckets[idx].fetch_add(1, std::memory_order_relaxed);
        _count.fetch_add(1, std::memory_order_relaxed);

        // sum 用 CAS loop (因为是 double)
        uint64_t old_bits = _sum_bits.load(std::memory_order_relaxed);
        double old_sum;
        std::memcpy(&old_sum, &old_bits, sizeof(old_sum));
        double new_sum = old_sum + value;
        uint64_t new_bits;
        std::memcpy(&new_bits, &new_sum, sizeof(new_bits));
        while (!_sum_bits.compare_exchange_weak(old_bits, new_bits,
                    std::memory_order_relaxed)) {
            std::memcpy(&old_sum, &old_bits, sizeof(old_sum));
            new_sum = old_sum + value;
            std::memcpy(&new_bits, &new_sum, sizeof(new_bits));
        }
    }

    // ─── 快照 ───
    struct Snapshot {
        std::vector<double> boundaries;
        std::vector<uint64_t> bucket_counts; // 累积值 (Prometheus 风格)
        uint64_t count;
        double sum;

        /**
         * 通过线性插值计算百分位数
         *
         * @param quantile 百分位 (0.0 ~ 1.0), e.g. 0.5 = P50
         * @return 估算值
         */
        double percentile(double quantile) const {
            if (count == 0) return 0.0;

            uint64_t target = static_cast<uint64_t>(quantile * count);
            if (target == 0) target = 1;

            for (size_t i = 0; i < bucket_counts.size(); ++i) {
                if (bucket_counts[i] >= target) {
                    if (i == 0) {
                        // 在第一个桶内，假设均匀分布在 [0, boundaries[0]]
                        if (bucket_counts[0] == 0) return 0.0;
                        return boundaries[0] * static_cast<double>(target) / bucket_counts[0];
                    }
                    // 在第 i 个桶内：线性插值
                    uint64_t prev_count = bucket_counts[i - 1];
                    uint64_t curr_count = bucket_counts[i] - prev_count;
                    if (curr_count == 0) {
                        return (i < boundaries.size()) ? boundaries[i] : boundaries.back();
                    }
                    double lower = boundaries[i - 1];
                    double upper = (i < boundaries.size()) ? boundaries[i]
                                   : boundaries.back() * 2.0; // +Inf 桶估算
                    double fraction = static_cast<double>(target - prev_count) / curr_count;
                    return lower + (upper - lower) * fraction;
                }
            }
            // 超出所有桶
            return boundaries.empty() ? 0.0 : boundaries.back();
        }
    };

    /**
     * 获取当前快照
     *
     * 线程安全: 是（每个字段独立 atomic load）
     * 注意: 快照不是全局一致的（各字段读取之间可能有新的 observe），
     *       但对于监控用途足够准确。
     */
    Snapshot snapshot() const {
        Snapshot snap;
        snap.boundaries = _boundaries;
        snap.count = _count.load(std::memory_order_relaxed);

        uint64_t sum_bits = _sum_bits.load(std::memory_order_relaxed);
        std::memcpy(&snap.sum, &sum_bits, sizeof(snap.sum));

        // 累积计数 (Prometheus 要求 le 桶是累积的)
        snap.bucket_counts.resize(_bucket_count);
        uint64_t cumulative = 0;
        for (size_t i = 0; i < _bucket_count; ++i) {
            cumulative += _buckets[i].load(std::memory_order_relaxed);
            snap.bucket_counts[i] = cumulative;
        }

        return snap;
    }

    const MetricMeta &meta() const { return _meta; }
    const std::vector<double> &boundaries() const { return _boundaries; }
    size_t bucket_count() const { return _bucket_count; }

private:
    MetricMeta _meta;
    std::vector<double> _boundaries;
    size_t _bucket_count;

    // 每个桶独占缓存行以避免 false sharing
    // MAX_BUCKETS=32 × 64 bytes = 2KB，对于静态分配可接受
    struct alignas(64) AlignedAtomic {
        std::atomic<uint64_t> value{0};
        void store(uint64_t v, std::memory_order order) { value.store(v, order); }
        uint64_t load(std::memory_order order) const { return value.load(order); }
        uint64_t fetch_add(uint64_t v, std::memory_order order) { return value.fetch_add(v, order); }
    };
    std::array<AlignedAtomic, MAX_BUCKETS> _buckets;

    alignas(64) std::atomic<uint64_t> _count{0};
    alignas(64) std::atomic<uint64_t> _sum_bits{0}; // bit-punned double
};

} // namespace neustack::telemetry


#endif