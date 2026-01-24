/**
 * @file main.cpp
 * @brief NeuStack HAL test program
 */

#include "neustack/hal/device.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>

static volatile bool running = true;

void signal_handler(int) {
    running = false;
}

// 打印 hex dump
void hexdump(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i % 16 == 0) std::printf("%04zx: ", i);
        std::printf("%02x ", data[i]);
        if (i % 16 == 15 || i == len - 1) std::printf("\n");
    }
}

int main() {
    std::printf("NeuStack v0.1.0 - HAL Test\n");
    std::printf("==========================\n\n");

    // 注册信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 创建设备
    auto device = NetDevice::create();
    if (!device) {
        std::fprintf(stderr, "Failed to create device\n");
        return EXIT_FAILURE;
    }

    // 打开设备
    if (device->open() < 0) {
        std::fprintf(stderr, "Failed to open device (need sudo?)\n");
        return EXIT_FAILURE;
    }

    std::printf("Device created: %s (fd=%d)\n", device->get_name().c_str(), device->get_fd());
    std::printf("\n");
    std::printf("Now configure the interface in another terminal:\n");
    std::printf("  sudo ifconfig %s 10.0.0.1 10.0.0.2 up\n", device->get_name().c_str());
    std::printf("\n");
    std::printf("Then test with:\n");
    std::printf("  ping 10.0.0.2\n");
    std::printf("\n");
    std::printf("Waiting for packets... (Ctrl+C to exit)\n\n");

    uint8_t buf[2048];
    int pkt_count = 0;

    while (running) {
        ssize_t n = device->recv(buf, sizeof(buf), 1000);  // 1秒超时

        if (n > 0) {
            pkt_count++;
            std::printf("[#%d] Received %zd bytes:\n", pkt_count, n);
            hexdump(buf, n > 64 ? 64 : n);  // 最多显示 64 字节
            if (n > 64) std::printf("  ... (%zd more bytes)\n", n - 64);
            std::printf("\n");
        } else if (n < 0) {
            std::perror("recv error");
            break;
        }
        // n == 0 是超时，继续循环
    }

    std::printf("\nShutting down...\n");
    device->close();
    std::printf("Total packets received: %d\n", pkt_count);

    return EXIT_SUCCESS;
}
