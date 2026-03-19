#ifndef NEUSTACK_HAL_DEVICE_HPP
#define NEUSTACK_HAL_DEVICE_HPP

// Platform-specific ssize_t
#if defined(_WIN32) || defined(_WIN64)
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
#else
    #include <sys/types.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace neustack {

/**
 * 包描述符 — 描述一个待处理/待发送的网络包
 *
 * 对于 TUN 设备：data 指向临时缓冲区，需要在处理前拷贝
 * 对于 AF_XDP：data 指向 UMEM frame，生命周期由 UMEM 管理
 */
struct PacketDesc {
    uint8_t* data;       // 包数据指针
    uint32_t len;        // 包长度
    uint32_t addr;       // UMEM frame 地址偏移 (AF_XDP 用；TUN 设为 0)
    uint16_t port_id;    // 接收队列 ID (多队列用；单队列设为 0)
    uint16_t flags;      // 保留标志位

    // 标志位定义
    static constexpr uint16_t FLAG_ZEROCOPY = 0x0001;  // 数据在 UMEM 中
    static constexpr uint16_t FLAG_NEED_FREE = 0x0002; // 处理完需要释放

    bool is_zerocopy() const { return flags & FLAG_ZEROCOPY; }
};

/**
 * 批量收发操作结果
 */
struct BatchResult {
    uint32_t count;      // 实际处理的包数
    uint32_t dropped;    // 丢弃的包数（ring 满等原因）
};

/**
 * @brief Abstract base class for network devices (HAL)
 *
 * Platform-specific implementations:
 * - macOS: utun device
 * - Linux: TUN/TAP device
 * - Windows: Wintun device
 */
class NetDevice {
public:
    virtual ~NetDevice() = default;

    // Lifecycle
    virtual int open() = 0;
    virtual int close() = 0;

    // I/O
    virtual ssize_t send(const uint8_t* data, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len, int timeout_ms = -1) = 0;

    // Properties
    virtual int get_fd() const = 0;
    virtual std::string get_name() const = 0;

    /**
     * 能力查询：是否支持 zero-copy 模式
     *
     * TUN/utun/Wintun 返回 false
     * AF_XDP 返回 true
     */
    virtual bool supports_zero_copy() const { return false; }

    /**
     * 能力查询：是否支持批量收发
     *
     * 即使 TUN 不支持 zero-copy，也可以支持批量模式
     * （内部循环调用 recv，减少事件循环开销）
     */
    virtual bool supports_batch() const { return false; }

    /**
     * 批量接收包
     *
     * @param descs  输出数组，调用者分配
     * @param max_count  最多接收几个包
     * @return 实际接收的包数 (0 = 无数据)
     *
     * 默认实现：调用 recv() 接收一个包
     */
    virtual uint32_t recv_batch(PacketDesc* descs, uint32_t max_count);

    /**
     * 批量发送包
     *
     * @param descs  待发送的包描述符数组
     * @param count  包数
     * @return 实际发送的包数
     *
     * 默认实现：循环调用 send()
     */
    virtual uint32_t send_batch(const PacketDesc* descs, uint32_t count);

    /**
     * 释放已处理的接收包
     *
     * 对于 zero-copy 模式，将 UMEM frame 归还到 fill ring
     * 对于 copy 模式，无操作（默认实现）
     *
     * @param descs  已处理完的包描述符数组
     * @param count  包数
     */
    virtual void release_rx(const PacketDesc* descs, uint32_t count) {
        (void)descs; (void)count;  // TUN 模式不需要释放
    }

    /**
     * 统一的事件等待
     *
     * 等待设备可读、可写或超时
     * 替代原来 recv(timeout_ms) 中的阻塞等待语义
     *
     * @param timeout_ms  超时时间 (-1 = 永久阻塞, 0 = 非阻塞)
     * @return 事件位掩码 (POLL_RX | POLL_TX | POLL_ERROR)
     */
    static constexpr int POLL_RX    = 0x01;
    static constexpr int POLL_TX    = 0x02;
    static constexpr int POLL_ERROR = 0x04;
    virtual int poll(int timeout_ms) {
        // 默认实现：用 recv(timeout) 模拟
        // 返回 POLL_RX（假设总是可读）
        (void)timeout_ms;
        return POLL_RX;
    }

    // Factory method (implemented in device.cpp)
    static std::unique_ptr<NetDevice> create();
    static std::unique_ptr<NetDevice> create(const std::string &type,
                                             const std::string &ifname = "");

    // Non-copyable
    NetDevice(const NetDevice&) = delete;
    NetDevice& operator=(const NetDevice&) = delete;

protected:
    NetDevice() = default;
};

} // namespace neustack

#endif // NEUSTACK_HAL_DEVICE_HPP
