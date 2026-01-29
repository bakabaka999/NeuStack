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

    // Factory method (implemented in device.cpp)
    static std::unique_ptr<NetDevice> create();

    // Non-copyable
    NetDevice(const NetDevice&) = delete;
    NetDevice& operator=(const NetDevice&) = delete;

protected:
    NetDevice() = default;
};

} // namespace neustack

#endif // NEUSTACK_HAL_DEVICE_HPP
