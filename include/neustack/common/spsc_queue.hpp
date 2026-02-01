#ifndef NEUSTACK_COMMON_SPSC_QUEUE_HPP
#define NEUSTACK_COMMON_SPSC_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace neustack {

template <typename T, size_t N> // N 必须是 2 的幂
class SPSCQueue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SPSCQueue element must be trivially copyable");
    static_assert((N & (N - 1)) == 0,
                  "SPSCQueue size must be power of 2");

public:
    SPSCQueue() : _read_pos(0), _write_pos(0) {}

    /**
     * 生产者: 尝试写入 (智能面线程调用)
     * 成功返回true，满了返回false
     */
    bool try_push(const T &item) {
        size_t write = _write_pos.load(std::memory_order_relaxed); // 不需同步操作
        size_t read = _read_pos.load(std::memory_order_acquire);   // 需要同步

        if (write - read >= N) return false; // 缓冲区满了

        _buffer[write & (N - 1)] = item; // 写数据（位运算实现环形）（N必须是2的幂）
        _write_pos.store(write + 1, std::memory_order_release); // 发布数据可见性
        return true;
    }

    /**
     * 消费者: 尝试读取 (数据面线程调用)
     */
    bool try_pop(T& item)  {
        // 消费者对 read 和 write 的需求自然与生产者相反
        size_t read = _read_pos.load(std::memory_order_relaxed);
        size_t write = _write_pos.load(std::memory_order_acquire);

        if (read == write) return false; // 缓冲区空了

        item = _buffer[read & (N - 1)];
        _read_pos.store(read + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return _read_pos.load(std::memory_order_acquire)
            == _write_pos.load(std::memory_order_acquire);
    }

    size_t size() const {
        return _write_pos.load(std::memory_order_acquire)
             - _read_pos.load(std::memory_order_acquire);
    }

private:
    std::array<T, N> _buffer; // 因为大小固定可以使用array，分配在栈上，比vector更优秀
    // 强制吧read和write的位置放在不同的cache line，防止false sharing错误
    alignas(64) std::atomic<size_t> _read_pos;  // 读指针
    alignas(64) std::atomic<size_t> _write_pos; // 写指针
};

} // namespace neustack

#endif // NEUSTACK_COMMON_SPSC_QUEUE_HPP