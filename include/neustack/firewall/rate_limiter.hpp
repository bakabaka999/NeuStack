#ifndef NEUSTACK_FIREWALL_RATE_LIMITER_HPP
#define NEUSTACK_FIREWALL_RATE_LIMITER_HPP

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <chrono>

namespace neustack {

/**
 * 令牌桶限速器 - 基于 IP 的 PPS 限制
 *
 * 算法: Token Bucket + EWMA PPS 估算
 * - 每个 IP 维护一个桶
 * - 桶以固定速率填充令牌
 * - 每个包消耗一个令牌
 * - 桶空时拒绝
 * - PPS 使用指数加权移动平均 (EWMA) 平滑估算
 *
 * 内存考量:
 * - 使用侵入式双向链表实现 O(1) LRU 驱逐
 * - 最多跟踪 MAX_TRACKED_IPS 个 IP
 */
class RateLimiter {
public:
    // ─── 配置常量 ───
    static constexpr size_t MAX_TRACKED_IPS = 65536;   // 最多跟踪的 IP 数
    static constexpr uint32_t DEFAULT_PPS = 1000;       // 默认 PPS 限制
    static constexpr uint32_t DEFAULT_BURST = 100;      // 默认突发容量
    static constexpr double EWMA_ALPHA = 0.125;         // EWMA 平滑系数 (类似 TCP RTT)

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
        uint32_t current_pps;   // 当前估算 PPS (EWMA)
        uint32_t limit_pps;     // 限制 PPS
        uint32_t tokens_left;   // 剩余令牌
    };

    explicit RateLimiter(const Config& config = {})
        : _config(config)
        , _lru_head(nullptr)
        , _lru_tail(nullptr)
    {}

    ~RateLimiter() = default;

    // 不可拷贝 (侵入式链表指针语义)
    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // 可移动
    RateLimiter(RateLimiter&& other) noexcept
        : _config(other._config)
        , _buckets(std::move(other._buckets))
        , _lru_head(other._lru_head)
        , _lru_tail(other._lru_tail)
    {
        other._lru_head = nullptr;
        other._lru_tail = nullptr;
    }

    RateLimiter& operator=(RateLimiter&& other) noexcept {
        if (this != &other) {
            _config = other._config;
            _buckets = std::move(other._buckets);
            _lru_head = other._lru_head;
            _lru_tail = other._lru_tail;
            other._lru_head = nullptr;
            other._lru_tail = nullptr;
        }
        return *this;
    }

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

        // EWMA 平滑 PPS 估算
        if (elapsed_us > 0) {
            double instant_pps = 1000000.0 / static_cast<double>(elapsed_us);
            bucket.ewma_pps = EWMA_ALPHA * instant_pps
                            + (1.0 - EWMA_ALPHA) * bucket.ewma_pps;
        }

        // 提升到 LRU 尾部 (最近访问)
        lru_move_to_tail(&bucket);

        uint32_t current_pps = static_cast<uint32_t>(bucket.ewma_pps);

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
        auto it = _buckets.find(ip);
        if (it != _buckets.end()) {
            lru_remove(it->second.get());
            _buckets.erase(it);
        }
    }

    /**
     * @brief 清空所有限速状态
     */
    void clear() {
        _buckets.clear();
        _lru_head = nullptr;
        _lru_tail = nullptr;
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
        double   ewma_pps;       // EWMA 平滑后的 PPS 估算
        uint32_t ip;             // 所属 IP (用于驱逐时反查)
        Bucket*  lru_prev;       // LRU 双向链表 - 前驱
        Bucket*  lru_next;       // LRU 双向链表 - 后继
    };

    // ─── LRU 链表操作 (全部 O(1)) ───

    void lru_remove(Bucket* b) {
        if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
        else             _lru_head = b->lru_next;

        if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
        else             _lru_tail = b->lru_prev;

        b->lru_prev = nullptr;
        b->lru_next = nullptr;
    }

    void lru_push_back(Bucket* b) {
        b->lru_prev = _lru_tail;
        b->lru_next = nullptr;
        if (_lru_tail) _lru_tail->lru_next = b;
        else           _lru_head = b;
        _lru_tail = b;
    }

    void lru_move_to_tail(Bucket* b) {
        if (b == _lru_tail) return;  // 已经在尾部
        lru_remove(b);
        lru_push_back(b);
    }

    // ─── 桶管理 ───

    // 注意: _buckets 存储 unique_ptr<Bucket>，避免 rehash 导致 Bucket 地址变化
    // 从而使 LRU 链表指针失效 (之前直接存 Bucket 值会在 rehash 时 crash)

    Bucket& get_or_create_bucket(uint32_t ip, uint64_t now_us) {
        auto it = _buckets.find(ip);
        if (it != _buckets.end()) {
            return *it->second;
        }

        // 如果超过最大跟踪数，从 LRU 头部驱逐 1/4
        if (_buckets.size() >= MAX_TRACKED_IPS) {
            evict_lru();
        }

        // 创建新桶 (heap-allocated, 地址稳定)
        auto bucket = std::make_unique<Bucket>(Bucket{
            .tokens = _config.burst_size,
            .last_update_us = now_us,
            .ewma_pps = 0.0,
            .ip = ip,
            .lru_prev = nullptr,
            .lru_next = nullptr
        });
        Bucket* raw = bucket.get();
        _buckets.emplace(ip, std::move(bucket));
        lru_push_back(raw);
        return *raw;
    }

    void evict_lru() {
        // 从链表头部 (最久未访问) 驱逐 1/4，O(K)
        size_t to_remove = _buckets.size() / 4;
        if (to_remove == 0) to_remove = 1;

        for (size_t i = 0; i < to_remove && _lru_head; ++i) {
            Bucket* victim = _lru_head;
            uint32_t victim_ip = victim->ip;
            lru_remove(victim);
            _buckets.erase(victim_ip);  // unique_ptr 自动释放
        }
    }

    Config _config;
    std::unordered_map<uint32_t, std::unique_ptr<Bucket>> _buckets;
    Bucket* _lru_head;  // 最久未访问
    Bucket* _lru_tail;  // 最近访问
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_RATE_LIMITER_HPP
