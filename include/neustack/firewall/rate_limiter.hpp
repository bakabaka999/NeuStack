#ifndef NEUSTACK_FIREWALL_RATE_LIMITER_HPP
#define NEUSTACK_FIREWALL_RATE_LIMITER_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>

namespace neustack {

/**
 * 令牌桶限速器 - 基于 IP 的 PPS 限制
 *
 * 算法: Token Bucket
 * - 每个 IP 维护一个桶
 * - 桶以固定速率填充令牌
 * - 每个包消耗一个令牌
 * - 桶空时拒绝
 *
 * 内存考量:
 * - 使用 LRU 清理机制防止内存膨胀
 * - 最多跟踪 MAX_TRACKED_IPS 个 IP
 */
class RateLimiter {
public:
    // ─── 配置常量 ───
    static constexpr size_t MAX_TRACKED_IPS = 65536;   // 最多跟踪的 IP 数
    static constexpr uint32_t DEFAULT_PPS = 1000;       // 默认 PPS 限制
    static constexpr uint32_t DEFAULT_BURST = 100;      // 默认突发容量

    struct Config {
        uint32_t packets_per_second;  // 每秒允许的包数
        uint32_t burst_size;          // 突发容量 (桶大小)
        bool enabled;                 // 是否启用限速
        
        Config() 
            : packets_per_second(DEFAULT_PPS)
            , burst_size(DEFAULT_BURST)
            , enabled(false) 
        {}
    };

    struct Result {
        bool allowed;           // 是否放行
        uint32_t current_pps;   // 当前估算 PPS
        uint32_t limit_pps;     // 限制 PPS
        uint32_t tokens_left;   // 剩余令牌
    };

    explicit RateLimiter(const Config& config = {}) : _config(config) {}

    /**
     * @brief 检查并消耗令牌
     * 
     * @param ip 源 IP 地址
     * @param now_us 当前时间 (微秒)
     * @return Result 限速结果
     */
    Result check(uint32_t ip, uint64_t now_us) {
        if (!_config.enabled) {
            return { true, 0, _config.packets_per_second, _config.burst_size };
        }

        auto& bucket = get_or_create_bucket(ip, now_us);
        
        // 计算自上次更新以来应该添加的令牌
        uint64_t elapsed_us = now_us - bucket.last_update_us;
        uint64_t tokens_to_add = (elapsed_us * _config.packets_per_second) / 1000000ULL;
        
        // 填充令牌 (不超过 burst_size)
        bucket.tokens = static_cast<uint32_t>(
            std::min<uint64_t>(bucket.tokens + tokens_to_add, _config.burst_size)
        );
        bucket.last_update_us = now_us;

        // 更新包计数 (用于 PPS 估算)
        bucket.packet_count++;
        
        // 估算当前 PPS (基于最近 1 秒的数据)
        uint32_t current_pps = 0;
        if (elapsed_us > 0) {
            // 简单的滑动窗口估算
            uint64_t window_us = std::min<uint64_t>(elapsed_us, 1000000ULL);
            current_pps = static_cast<uint32_t>(
                (bucket.packet_count * 1000000ULL) / std::max<uint64_t>(window_us, 1)
            );
        }

        // 检查是否有令牌可用
        if (bucket.tokens > 0) {
            bucket.tokens--;
            return { true, current_pps, _config.packets_per_second, bucket.tokens };
        }

        // 令牌耗尽，拒绝
        return { false, current_pps, _config.packets_per_second, 0 };
    }

    /**
     * @brief 重置特定 IP 的限速状态
     */
    void reset(uint32_t ip) {
        _buckets.erase(ip);
    }

    /**
     * @brief 清空所有限速状态
     */
    void clear() {
        _buckets.clear();
    }

    /**
     * @brief 获取当前跟踪的 IP 数量
     */
    size_t tracked_count() const {
        return _buckets.size();
    }

    // ─── 配置 API ───
    
    void set_enabled(bool enabled) { _config.enabled = enabled; }
    bool enabled() const { return _config.enabled; }
    
    void set_rate(uint32_t pps, uint32_t burst) {
        _config.packets_per_second = pps;
        _config.burst_size = burst;
    }

    const Config& config() const { return _config; }

private:
    struct Bucket {
        uint32_t tokens;         // 当前令牌数
        uint64_t last_update_us; // 上次更新时间
        uint64_t packet_count;   // 包计数 (用于 PPS 估算)
        uint64_t created_us;     // 创建时间 (用于 LRU)
    };

    Bucket& get_or_create_bucket(uint32_t ip, uint64_t now_us) {
        auto it = _buckets.find(ip);
        if (it != _buckets.end()) {
            return it->second;
        }

        // 如果超过最大跟踪数，清理最旧的 1/4
        if (_buckets.size() >= MAX_TRACKED_IPS) {
            evict_oldest();
        }

        // 创建新桶，初始令牌数 = burst_size
        auto [new_it, _] = _buckets.emplace(ip, Bucket{
            .tokens = _config.burst_size,
            .last_update_us = now_us,
            .packet_count = 0,
            .created_us = now_us
        });
        return new_it->second;
    }

    void evict_oldest() {
        // 简单策略：清理 1/4 的桶 (按创建时间)
        // 生产环境可用更精细的 LRU
        size_t to_remove = _buckets.size() / 4;
        if (to_remove == 0) to_remove = 1;

        // 找出最旧的 N 个
        std::vector<std::pair<uint64_t, uint32_t>> age_ip;
        age_ip.reserve(_buckets.size());
        for (const auto& [ip, bucket] : _buckets) {
            age_ip.emplace_back(bucket.created_us, ip);
        }
        std::partial_sort(age_ip.begin(), age_ip.begin() + to_remove, age_ip.end());

        for (size_t i = 0; i < to_remove; ++i) {
            _buckets.erase(age_ip[i].second);
        }
    }

    Config _config;
    std::unordered_map<uint32_t, Bucket> _buckets;
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_RATE_LIMITER_HPP
