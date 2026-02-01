#ifndef NEUSTACK_COMMON_RING_BUFFER_HPP
#define NEUSTACK_COMMON_RING_BUFFER_HPP

#include <array>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <type_traits>

namespace neustack {

// ============================================================================
// StreamBuffer - TCP 收发缓冲区 (单线程，字节流优化)
// ============================================================================

/**
 * 高性能字节流环形缓冲区
 *
 * 特点:
 * - 固定大小，零动态分配
 * - O(1) 头部消费（无内存拷贝）
 * - 支持连续块读取
 * - 单线程使用
 *
 * 用于: TCP send_buffer / recv_buffer
 */
class StreamBuffer {
public:
    explicit StreamBuffer(size_t capacity)
        : _buffer(capacity)
        , _capacity(capacity)
        , _read_pos(0)
        , _write_pos(0)
        , _size(0) {}

    // 默认 64KB 缓冲区
    StreamBuffer() : StreamBuffer(65536) {}

    // ─── 容量查询 ───

    size_t size() const { return _size; }
    size_t capacity() const { return _capacity; }
    size_t available() const { return _capacity - _size; }
    bool empty() const { return _size == 0; }
    bool full() const { return _size == _capacity; }

    // ─── 写入操作 ───

    /**
     * 写入数据到缓冲区尾部
     * @return 实际写入的字节数
     */
    size_t write(const uint8_t* data, size_t len) {
        size_t to_write = std::min(len, available());
        if (to_write == 0) return 0;

        // 可能需要分两段写入（回绕）
        size_t first_chunk = std::min(to_write, _capacity - _write_pos);
        std::memcpy(_buffer.data() + _write_pos, data, first_chunk);

        if (to_write > first_chunk) {
            // 第二段：从头开始
            std::memcpy(_buffer.data(), data + first_chunk, to_write - first_chunk);
        }

        _write_pos = (_write_pos + to_write) % _capacity;
        _size += to_write;
        return to_write;
    }

    // ─── 读取操作 ───

    /**
     * 读取数据（不移除）
     * @return 实际读取的字节数
     */
    size_t peek(uint8_t* dest, size_t len) const {
        size_t to_read = std::min(len, _size);
        if (to_read == 0) return 0;

        size_t first_chunk = std::min(to_read, _capacity - _read_pos);
        std::memcpy(dest, _buffer.data() + _read_pos, first_chunk);

        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, _buffer.data(), to_read - first_chunk);
        }

        return to_read;
    }

    /**
     * 读取数据并移除
     * @return 实际读取的字节数
     */
    size_t read(uint8_t* dest, size_t len) {
        size_t bytes_read = peek(dest, len);
        consume(bytes_read);
        return bytes_read;
    }

    /**
     * 消费（丢弃）头部数据，不拷贝
     * @return 实际消费的字节数
     */
    size_t consume(size_t len) {
        size_t to_consume = std::min(len, _size);
        _read_pos = (_read_pos + to_consume) % _capacity;
        _size -= to_consume;
        return to_consume;
    }

    // ─── 连续块访问（零拷贝优化）───

    /**
     * 获取可连续读取的第一段
     * 用于直接传递给网络发送，避免拷贝
     */
    struct Span {
        const uint8_t* data;
        size_t len;
    };

    Span peek_contiguous() const {
        if (_size == 0) return {nullptr, 0};
        // 第一段：从 read_pos 到 buffer 末尾或 write_pos
        size_t first_chunk = std::min(_size, _capacity - _read_pos);
        return {_buffer.data() + _read_pos, first_chunk};
    }

    /**
     * 获取可连续写入的空间
     * 用于直接从网络接收，避免拷贝
     */
    struct MutableSpan {
        uint8_t* data;
        size_t len;
    };

    MutableSpan write_contiguous() {
        if (available() == 0) return {nullptr, 0};
        size_t first_chunk = std::min(available(), _capacity - _write_pos);
        return {_buffer.data() + _write_pos, first_chunk};
    }

    /**
     * 确认写入（配合 write_contiguous 使用）
     */
    void commit_write(size_t len) {
        size_t to_commit = std::min(len, available());
        _write_pos = (_write_pos + to_commit) % _capacity;
        _size += to_commit;
    }

    // ─── 特殊操作 ───

    /**
     * 从指定偏移读取（用于重传时读取特定位置的数据）
     */
    size_t peek_at(size_t offset, uint8_t* dest, size_t len) const {
        if (offset >= _size) return 0;
        size_t available_from_offset = _size - offset;
        size_t to_read = std::min(len, available_from_offset);

        size_t actual_pos = (_read_pos + offset) % _capacity;
        size_t first_chunk = std::min(to_read, _capacity - actual_pos);
        std::memcpy(dest, _buffer.data() + actual_pos, first_chunk);

        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, _buffer.data(), to_read - first_chunk);
        }

        return to_read;
    }

    void clear() {
        _read_pos = 0;
        _write_pos = 0;
        _size = 0;
    }

private:
    std::vector<uint8_t> _buffer;  // 使用 vector 是因为大小可配置
    size_t _capacity;
    size_t _read_pos;
    size_t _write_pos;
    size_t _size;
};

// ============================================================================
// MetricsBuffer - 指标采集缓冲区 (多线程安全，固定元素)
// ============================================================================

/**
 * 多线程安全的采样数据环形缓冲区
 *
 * 特点:
 * - 编译期固定大小
 * - 单生产者多消费者 (SPMC)
 * - 无锁写入
 * - 新数据覆盖旧数据
 *
 * 用于: AI 指标采集 (TCP线程写入, AI线程读取)
 */
template <typename T, size_t N>
class MetricsBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "MetricsBuffer element must be trivially copyable");
    static_assert((N & (N - 1)) == 0,
                  "MetricsBuffer size must be power of 2 for fast modulo");

public:
    MetricsBuffer() : _write_pos(0) {}

    // ─── 生产者接口 (单线程调用) ───

    /**
     * 写入一个样本（无锁）
     */
    void push(const T& sample) {
        size_t pos = _write_pos.load(std::memory_order_relaxed);
        _buffer[pos & (N - 1)] = sample;
        _write_pos.store(pos + 1, std::memory_order_release);
    }

    // ─── 消费者接口 (可多线程调用) ───

    /**
     * 获取最新样本
     */
    T latest() const {
        size_t pos = _write_pos.load(std::memory_order_acquire);
        if (pos == 0) return T{};
        return _buffer[(pos - 1) & (N - 1)];
    }

    /**
     * 获取最近 n 个样本（从旧到新）
     */
    std::vector<T> recent(size_t n) const {
        std::vector<T> result;
        size_t end = _write_pos.load(std::memory_order_acquire);
        size_t count = std::min(n, std::min(end, N));
        size_t start = end - count;

        result.reserve(count);
        for (size_t i = start; i < end; ++i) {
            result.push_back(_buffer[i & (N - 1)]);
        }
        return result;
    }

    /**
     * 获取所有有效样本
     */
    std::vector<T> all() const {
        return recent(N);
    }

    // ─── 状态查询 ───

    size_t count() const {
        size_t pos = _write_pos.load(std::memory_order_acquire);
        return std::min(pos, N);
    }

    size_t total_pushed() const {
        return _write_pos.load(std::memory_order_acquire);
    }

    bool empty() const {
        return _write_pos.load(std::memory_order_acquire) == 0;
    }

    static constexpr size_t capacity() { return N; }

    void clear() {
        _write_pos.store(0, std::memory_order_release);
    }

private:
    std::array<T, N> _buffer;
    std::atomic<size_t> _write_pos;
};

// ============================================================================
// 预定义特化
// ============================================================================

// TCP 缓冲区常用大小
inline StreamBuffer make_tcp_buffer(size_t size = 65536) {
    return StreamBuffer(size);
}

// Metrics 缓冲区常用大小 (1024 = 2^10)
template <typename T>
using MetricsBuffer1K = MetricsBuffer<T, 1024>;

template <typename T>
using MetricsBuffer4K = MetricsBuffer<T, 4096>;

} // namespace neustack

#endif // NEUSTACK_COMMON_RING_BUFFER_HPP
