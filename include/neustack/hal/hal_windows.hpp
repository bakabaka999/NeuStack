#ifndef NEUSTACK_HAL_WINDOWS_HPP
#define NEUSTACK_HAL_WINDOWS_HPP

#ifdef NEUSTACK_PLATFORM_WINDOWS

#include "neustack/hal/device.hpp"

namespace neustack {

// 源文件待实现

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
    int _fd = -1;
    std::string _name;
};

} // namespace neustack

#endif // NEUSTACK_PLATFORM_WINDOWS
#endif // NEUSTACK_HAL_WINDOWS_HPP
