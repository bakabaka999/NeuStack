#ifndef NEUSTACK_HAL_XDP_RING_HPP
#define NEUSTACK_HAL_XDP_RING_HPP

#include <cstdint>
#include <atomic>
#include <linux/if_xdp.h>

namespace neustack {
    
/**
 * XDP Ring — 用户态对内核共享 ring 的封装
 *
 * 这是一个 view 类（不拥有内存），指向 mmap 映射的内核 ring。
 * 提供类似 SPSCQueue 的 reserve/submit/peek/release 操作。
 *
 * 模板参数 T：描述符类型
 *   - Fill/Completion Ring: uint64_t (UMEM 地址偏移)
 *   - RX/TX Ring: struct xdp_desc { uint64_t addr; uint32_t len; uint32_t options; }
 */
template <class T>
class XdpRing {
public:
    XdpRing() = default;

    /**
     * 初始化 ring view
     * @param producer  生产者指针 (mmap 映射)
     * @param consumer  消费者指针 (mmap 映射)
     * @param descs     描述符数组 (mmap 映射)
     * @param size      ring 大小 (必须是 2 的幂)
     * @param flags     ring 标志指针 (可选)
     */
    void init(uint32_t* producer, uint32_t* consumer,
              T* descs, uint32_t size, uint32_t* flags = nullptr) {
        _producer = producer;
        _consumer = consumer;
        _descs = descs;
        _mask = size - 1;
        _size = size;
        _flags = flags;
    }

    // ─── 生产者操作 (写入 ring) ───

    /**
     * 预留 n 个槽位
     * @param count  请求的槽位数
     * @return 实际可用的槽位数
     *
     * 调用后通过 ring_at(idx) 写入数据，然后 submit(n) 提交
     */
    uint32_t reserve(uint32_t count) {
        uint32_t prod = __atomic_load_n(_producer, __ATOMIC_RELAXED);
        uint32_t cons = __atomic_load_n(_consumer, __ATOMIC_ACQUIRE);
        uint32_t free = _size - (prod - cons);
        _cached_prod = prod;
        return std::min(count, free);
    }

    /**
     * 获取预留槽位的引用
     * @param idx  相对于 reserve 起始位置的偏移
     */
    T& ring_at(uint32_t idx) {
        return _descs[(_cached_prod + idx) & _mask];
    }

    /**
     * 提交已写入的描述符
     * @param count  提交的数量 (必须 <= reserve 返回值)
     */
    void submit(uint32_t count) {
        __atomic_store_n(_producer, _cached_prod + count, __ATOMIC_RELEASE);
    }

    // ─── 消费者操作 (读取 ring) ───

    /**
     * 查看可消费的描述符数量
     * @param count  最多查看几个
     * @return 实际可读的数量
     *
     * 调用后通过 ring_at_consumer(idx) 读取，然后 release(n) 释放
     */
    uint32_t peek(uint32_t count) {
        uint32_t cons = __atomic_load_n(_consumer, __ATOMIC_RELAXED);
        uint32_t prod = __atomic_load_n(_producer, __ATOMIC_ACQUIRE);
        uint32_t available = prod - cons;
        _cached_cons = cons;
        return std::min(count, available);
    }

    /**
     * 获取消费位置的描述符引用
     */
    const T& ring_at_consumer(uint32_t idx) const {
        return _descs[(_cached_cons + idx) & _mask];
    }

    /**
     * 释放已消费的描述符
     */
    void release(uint32_t count) {
        __atomic_store_n(_consumer, _cached_cons + count, __ATOMIC_RELEASE);
    }

    // ─── 状态查询 ───

    bool needs_wakeup() const {
        return _flags && (*_flags & XDP_RING_NEED_WAKEUP);
    }

    uint32_t size() const { return _size; }

    bool empty() const {
        return __atomic_load_n(_producer, __ATOMIC_ACQUIRE)
            == __atomic_load_n(_consumer, __ATOMIC_ACQUIRE);
    }

private:
    uint32_t *_producer = nullptr;
    uint32_t *_consumer = nullptr;
    T *_descs = nullptr;
    uint32_t *_flags = nullptr;
    uint32_t _mask = 0;
    uint32_t _size = 0;
    uint32_t _cached_prod = 0; // 缓存生产者位置 (避免重复原子读)
    uint32_t _cached_cons = 0; // 缓存消费者位置
};

} // namespace neustack


#endif // NEUSTACK_HAL_XDP_RING_HPP