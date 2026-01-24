#ifndef NEUSTACK_HAL_MACOS_HPP
#define NEUSTACK_HAL_MACOS_HPP

#ifdef NEUSTACK_PLATFORM_MACOS

#include "neustack/hal/device.hpp"

class MacOSDevice : public NetDevice {
public:
    MacOSDevice() = default;
    ~MacOSDevice() override { close(); }

    int open() override;
    int close() override;
    ssize_t send(const uint8_t* data, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len, int timeout_ms = -1) override;
    int get_fd() const override;
    std::string get_name() const override;

private:
    int _fd = -1;
    std::string _name;
};

#endif // NEUSTACK_PLATFORM_MACOS
#endif // NEUSTACK_HAL_MACOS_HPP