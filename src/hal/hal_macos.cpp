/**
 * @file hal_macos.cpp
 * @brief macOS HAL implementation using utun device
 */

#ifdef NEUSTACK_PLATFORM_MACOS

#include "neustack/hal/hal_macos.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

int MacOSDevice::open() {
    // 创建控制 socket
    _fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (_fd < 0) {
        std::perror("socket(PF_SYSTEM)");
        return -1;
    }

    // 获取 utun 控制 ID
    struct ctl_info info{};
    std::strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));

    if (ioctl(_fd, CTLIOCGINFO, &info) < 0) {
        std::perror("ioctl(CTLIOCGINFO)");
        ::close(_fd);
        _fd = -1;
        return -1;
    }

    // 连接到 utun
    struct sockaddr_ctl sc{};
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = info.ctl_id;
    sc.sc_unit = 0;  // 系统自动分配编号

    if (connect(_fd, reinterpret_cast<struct sockaddr*>(&sc), sizeof(sc)) < 0) {
        std::perror("connect(utun)");
        ::close(_fd);
        _fd = -1;
        return -1;
    }

    // 获取分配的设备名
    char ifname[IFNAMSIZ];
    socklen_t ifname_len = sizeof(ifname);
    if (getsockopt(_fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &ifname_len) == 0) {
        _name = ifname;
    }

    // 设置非阻塞
    int flags = fcntl(_fd, F_GETFL, 0);
    fcntl(_fd, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

int MacOSDevice::close() {
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
        _name.clear();
    }
    return 0;
}

ssize_t MacOSDevice::send(const uint8_t* data, size_t len) {
    if (_fd < 0) return -1;

    // macOS utun 需要 4 字节协议族前缀
    constexpr size_t PREFIX_LEN = 4;
    uint8_t buf[PREFIX_LEN + 1500];  // MTU 通常 1500

    if (len > sizeof(buf) - PREFIX_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t proto = htonl(AF_INET);
    std::memcpy(buf, &proto, PREFIX_LEN);
    std::memcpy(buf + PREFIX_LEN, data, len);

    ssize_t n = write(_fd, buf, PREFIX_LEN + len);
    if (n < 0) return -1;

    // 返回实际发送的数据长度（不含前缀）
    return (n > static_cast<ssize_t>(PREFIX_LEN)) ? (n - PREFIX_LEN) : 0;
}

ssize_t MacOSDevice::recv(uint8_t* buf, size_t len, int timeout_ms) {
    if (_fd < 0) return -1;

    // 使用 poll 实现超时
    if (timeout_ms >= 0) {
        struct pollfd pfd{};
        pfd.fd = _fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) return -1;   // 错误
        if (ret == 0) return 0;   // 超时
    }

    constexpr size_t PREFIX_LEN = 4;
    uint8_t recv_buf[PREFIX_LEN + 1500];

    ssize_t n = read(_fd, recv_buf, sizeof(recv_buf));
    if (n < static_cast<ssize_t>(PREFIX_LEN)) {
        // EAGAIN/EWOULDBLOCK 表示无数据（非阻塞模式）
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    size_t payload_len = n - PREFIX_LEN;
    if (payload_len > len) payload_len = len;  // 截断

    std::memcpy(buf, recv_buf + PREFIX_LEN, payload_len);
    return static_cast<ssize_t>(payload_len);
}

int MacOSDevice::get_fd() const {
    return _fd;
}

std::string MacOSDevice::get_name() const {
    return _name;
}

#endif // NEUSTACK_PLATFORM_MACOS