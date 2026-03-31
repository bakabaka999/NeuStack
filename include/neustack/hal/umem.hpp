#ifndef NEUSTACK_HAL_UMEM_HPP
#define NEUSTACK_HAL_UMEM_HPP

#ifdef NEUSTACK_PLATFORM_LINUX

#include <cstdint>
#include <cstddef>
#include <vector>

namespace neustack {
    
/**
 * UMEM Frame 分配器
 *
 * 管理 UMEM 中 frame 的分配和回收。
 * 类似于 FixedPool，但操作的是 UMEM 偏移量而非指针。
 *
 * 线程安全：否（单线程使用，在事件循环线程中）
 */
class UmemFrameAllocator {
public:
    /**
     * @param frame_count  frame 总数
     * @param frame_size   每个 frame 的大小 (通常 4096)
     */
    UmemFrameAllocator(uint32_t frame_count, uint32_t frame_size)
        : _frame_size(frame_size)
    {
        // 初始化空闲列表：所有 frame 都可用
        _free_list.reserve(frame_count);
        for (uint32_t i = 0; i < frame_count; ++i) {
            _free_list.push_back(i * frame_size);
        }
    }

    /**
     * 分配一个 frame
     * @return frame 的 UMEM 偏移量，失败返回 INVALID_ADDR
     */
    uint64_t alloc() {
        if (_free_list.empty()) return INVALID_ADDR;
        uint64_t addr = _free_list.back();
        _free_list.pop_back();
        return addr;
    }

    /**
     * 批量分配 frame
     * @param addrs  输出数组
     * @param count  请求数量
     * @return 实际分配的数量
     */
    uint32_t alloc_batch(uint64_t *addrs, uint32_t count) {
        uint32_t n = std::min(count, static_cast<uint32_t>(_free_list.size()));
        for (uint32_t i = 0; i < n; i++) {
            addrs[i] = _free_list.back();
            _free_list.pop_back();
        }
        return n;
    }

    /**
     * 释放一个 frame
     * @param addr frame 的 UMEM 偏移量
     */
    void free(uint64_t addr) {
        _free_list.push_back(addr);
    }

    /**
     * 批量释放
     */
    void free_batch(const uint64_t *addrs, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            _free_list.push_back(addrs[i]);
        }
    }

    uint32_t available() const { return static_cast<uint32_t>(_free_list.size()); }
    uint32_t frame_size() const { return _frame_size; }

    static constexpr uint64_t INVALID_ADDR = UINT64_MAX;

private:
    uint32_t _frame_size;
    std::vector<uint64_t> _free_list; // LIFO 栈（cache友好）
};

/**
 * UMEM — AF_XDP 共享内存区域
 *
 * 封装 UMEM 的创建、注册、内存映射。
 * 提供 frame 分配/释放和地址-指针转换。
 */
class Umem {
public:
    struct Config {
        uint32_t frame_count = 4096;
        uint32_t frame_size = 4096;
        uint32_t headroom = 0;
        uint32_t flags = 0;
    };

    Umem();
    explicit Umem(const Config &config);
    ~Umem();

    // 禁止拷贝
    Umem(const Umem &) = delete;
    Umem &operator=(const Umem &) = delete;

    /**
     * 初始化：分配内存，创建 UMEM
     * @return 0 成功, -1 失败
     */
    int create();

    /**
     * 注册到 AF_XDP socket
     * @param xsk_fd XDP socket 文件描述符
     * @return 0 成功, -1 失败
     */
    int register_to_socket(int xsk_fd);

    // ─── Frame 管理 ───

    /** 分配一个 frame，返回 UMEM 偏移量 */
    uint64_t alloc_frame() { return _allocator.alloc(); }

    /** 批量分配 */
    uint32_t alloc_frames(uint64_t *addrs, uint32_t count) {
        return _allocator.alloc_batch(addrs, count);
    }

    /** 释放 frame */
    void free_frame(uint64_t addr) { _allocator.free(addr); }

    /** 批量释放 */
    void free_frames(const uint64_t* addrs, uint32_t count) {
        _allocator.free_batch(addrs, count);
    }

    // ─── 地址转换 ───

    /** UMEM 偏移量 → 数据指针 */
    uint8_t *addr_to_ptr(uint64_t addr) {
        return _buffer + addr;
    }

    /** 数据指针 → UMEM 偏移量 */
    uint64_t ptr_to_addr(const uint8_t* ptr) {
        return static_cast<uint64_t>(ptr - _buffer);
    }

    // ─── 属性 ───

    uint8_t *buffer() { return _buffer; }
    size_t buffer_size() const { return _buffer_size; }
    uint32_t frame_size() const { return _config.frame_size; }
    uint32_t frame_count() const { return _config.frame_count; }
    uint32_t available_frames() const { return _allocator.available(); }

private:
    Config _config;
    uint8_t *_buffer = nullptr;
    size_t _buffer_size = 0;
    UmemFrameAllocator _allocator;
};

} // namespace neustack

#endif // NEUSTACK_PLATFORM_LINUX
#endif // NEUSTACK_HAL_UMEM_HPP