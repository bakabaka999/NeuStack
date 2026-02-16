#ifndef NEUSTACK_HAL_WINDOWS_HPP
#define NEUSTACK_HAL_WINDOWS_HPP

#ifdef NEUSTACK_PLATFORM_WINDOWS

#include "neustack/hal/device.hpp"

namespace neustack {

/**
 * Windows HAL implementation using Wintun
 *
 * Wintun 是 WireGuard 项目的 L3 TUN 驱动，提供 ring buffer 收发 IP 包。
 * DLL 运行时加载，无编译期依赖。
 *
 * 参考: https://www.wintun.net/
 */
class WindowsDevice : public NetDevice {
public:
    WindowsDevice() = default;
    ~WindowsDevice() override { close(); }

    int open() override;
    int close() override;
    ssize_t send(const uint8_t *data, size_t len) override;
    ssize_t recv(uint8_t *buf, size_t len, int timeout_ms = -1) override;
    int get_fd() const override;
    std::string get_name() const override;

private:
    // Wintun 句柄 (opaque pointers, 实际类型在 cpp 中定义)
    void *_adapter = nullptr;
    void *_session = nullptr;
    void *_read_event = nullptr;  // WintunGetReadWaitEvent 返回的 HANDLE
    void *_wintun_dll = nullptr;  // DLL 模块句柄

    std::string _name;

    // Ring buffer 容量 (4 MB)
    static constexpr unsigned int RING_CAPACITY = 0x400000;

    // 加载 wintun.dll 并解析函数指针
    bool load_wintun_dll();

    // Wintun 函数指针 (运行时加载)
    void *_fn_create_adapter = nullptr;
    void *_fn_close_adapter = nullptr;
    void *_fn_start_session = nullptr;
    void *_fn_end_session = nullptr;
    void *_fn_get_read_wait_event = nullptr;
    void *_fn_receive_packet = nullptr;
    void *_fn_release_receive_packet = nullptr;
    void *_fn_allocate_send_packet = nullptr;
    void *_fn_send_packet = nullptr;
};

} // namespace neustack

#endif // NEUSTACK_PLATFORM_WINDOWS
#endif // NEUSTACK_HAL_WINDOWS_HPP
