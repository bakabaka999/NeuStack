#ifdef NEUSTACK_PLATFORM_LINUX

#include "neustack/hal/hal_linux.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace neustack;

int LinuxDevice::open()
{
    // 1. 打开 TUN 字符设备
    _fd = ::open("/dev/net/tun", O_RDWR);
    if (_fd < 0)
    {
        LOG_ERROR(HAL, "open(/dev/net/tun) failed: %s", std::strerror(errno));
        return -1;
    }

    // 2. 配置为 TUN 模式 (L3, 无协议信息前缀)
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    // IFF_TUN    = L3 隧道 (IP 包, 无以太网头)
    // IFF_NO_PI  = 不加协议信息前缀 (与 macOS utun 不同!)

    if (ioctl(_fd, TUNSETIFF, &ifr) < 0)
    {
        LOG_ERROR(HAL, "ioctl(TUNSETIFF) failed: %s", std::strerror(errno));
        ::close(_fd);
        _fd = -1;
        return -1;
    }

    _name = ifr.ifr_name; // 内核分配的名称, 如 "tun0"

    // 3. 设置非阻塞
    int flags = fcntl(_fd, F_GETFL, 0);
    fcntl(_fd, F_SETFL, flags | O_NONBLOCK);

    LOG_DEBUG(HAL, "TUN device opened: %s (fd=%d)", _name.c_str(), _fd);
    return 0;
}

int LinuxDevice::close()
{
    if (_fd >= 0)
    {
        LOG_DEBUG(HAL, "closing device %s", _name.c_str());
        ::close(_fd);
        _fd = -1;
        _name.clear();
    }
    return 0;
}

ssize_t LinuxDevice::send(const uint8_t *data, size_t len)
{
    if (_fd < 0)
        return -1;

    // Linux TUN + IFF_NO_PI: 直接写裸 IP 包, 无前缀
    ssize_t n = write(_fd, data, len);
    if (n < 0)
    {
        LOG_ERROR(HAL, "write failed: %s", std::strerror(errno));
        return -1;
    }

    LOG_TRACE(HAL, "sent %zd bytes", n);
    return n;
}

ssize_t LinuxDevice::recv(uint8_t *buf, size_t len, int timeout_ms)
{
    if (_fd < 0)
        return -1;

    // 使用 poll 实现超时
    if (timeout_ms >= 0)
    {
        struct pollfd pfd{};
        pfd.fd = _fd;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret < 0)
        {
            if (errno == EINTR)
                return 0;
            LOG_ERROR(HAL, "poll failed: %s", std::strerror(errno));
            return -1;
        }
        if (ret == 0)
            return 0; // 超时
    }

    // Linux TUN + IFF_NO_PI: 直接读裸 IP 包, 无前缀
    ssize_t n = read(_fd, buf, len);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        LOG_ERROR(HAL, "read failed: %s", std::strerror(errno));
        return -1;
    }

    LOG_TRACE(HAL, "received %zd bytes", n);
    return n;
}

int LinuxDevice::get_fd() const { return _fd; }
std::string LinuxDevice::get_name() const { return _name; }

#endif // NEUSTACK_PLATFORM_LINUX