#include "neustack/hal/device.hpp"
#include "neustack/common/log.hpp"

#if defined(NEUSTACK_PLATFORM_MACOS)
    #include "neustack/hal/hal_macos.hpp"
#elif defined(NEUSTACK_PLATFORM_LINUX)
    #include "neustack/hal/hal_linux.hpp"
    #if defined(NEUSTACK_ENABLE_AF_XDP)
        #include "neustack/hal/hal_linux_afxdp.hpp"
    #endif
#elif defined(NEUSTACK_PLATFORM_WINDOWS)
    #include "neustack/hal/hal_windows.hpp"
#endif

using namespace neustack;

uint32_t NetDevice::recv_batch(PacketDesc* descs, uint32_t max_count) {
    if (max_count == 0) return 0;

    // 分配临时缓冲区（栈上，TUN 模式用）
    // 注意：这个 buf 的生命周期必须覆盖调用者处理包的全过程
    // 对于 TUN 模式，调用者需要在 release_rx() 前完成处理
    thread_local uint8_t buf[2048];

    ssize_t n = recv(buf, sizeof(buf), 0);  // 非阻塞尝试
    if (n <= 0) return 0;

    descs[0].data = buf;
    descs[0].len = static_cast<uint32_t>(n);
    descs[0].addr = 0;
    descs[0].port_id = 0;
    descs[0].flags = 0;  // 非 zero-copy

    return 1;  // TUN 模式一次最多收一个包
}

uint32_t NetDevice::send_batch(const PacketDesc* descs, uint32_t count) {
    uint32_t sent = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ssize_t n = send(descs[i].data, descs[i].len);
        if (n > 0) ++sent;
    }
    return sent;
}

std::unique_ptr<NetDevice> NetDevice::create() {
#if defined(NEUSTACK_PLATFORM_MACOS)
    return std::make_unique<MacOSDevice>();
#elif defined(NEUSTACK_PLATFORM_LINUX)
    return std::make_unique<LinuxDevice>();
#elif defined(NEUSTACK_PLATFORM_WINDOWS)
    return std::make_unique<WindowsDevice>();
#else
    #error "Unsupported platform"
#endif
}

std::unique_ptr<NetDevice> NetDevice::create(const std::string& type) {
    if (type == "af_xdp") {
#if defined(NEUSTACK_PLATFORM_LINUX) && defined(NEUSTACK_ENABLE_AF_XDP)
        return std::make_unique<LinuxAFXDPDevice>();
#else
        LOG_ERROR(HAL, "AF_XDP not available (Linux + NEUSTACK_ENABLE_AF_XDP required)");
        return nullptr;
#endif
    }

    // 默认：使用平台原生 TUN 设备
    return NetDevice::create();
}
