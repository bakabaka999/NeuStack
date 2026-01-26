/**
 * @file main.cpp
 * @brief NeuStack - User-space TCP/IP Stack
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace neustack;

static volatile bool running = true;

void signal_handler(int) {
    running = false;
}

static void print_usage(const char* prog) {
    std::printf("Usage: %s [options]\n", prog);
    std::printf("Options:\n");
    std::printf("  -v          Verbose (DEBUG level)\n");
    std::printf("  -vv         Very verbose (TRACE level)\n");
    std::printf("  -q          Quiet (WARN level)\n");
    std::printf("  --no-color  Disable colored output\n");
    std::printf("  --no-time   Disable timestamps\n");
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    auto& logger = Logger::instance();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) {
            logger.set_level(LogLevel::DEBUG);
        } else if (std::strcmp(argv[i], "-vv") == 0) {
            logger.set_level(LogLevel::TRACE);
        } else if (std::strcmp(argv[i], "-q") == 0) {
            logger.set_level(LogLevel::WARN);
        } else if (std::strcmp(argv[i], "--no-color") == 0) {
            logger.set_color(false);
        } else if (std::strcmp(argv[i], "--no-time") == 0) {
            logger.set_timestamp(false);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    LOG_INFO(APP, "NeuStack v0.1.0 starting");

    // 注册信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 创建设备
    auto device = NetDevice::create();
    if (!device) {
        LOG_FATAL(HAL, "failed to create device");
        return EXIT_FAILURE;
    }

    // 打开设备
    if (device->open() < 0) {
        LOG_FATAL(HAL, "failed to open device (need sudo?)");
        return EXIT_FAILURE;
    }

    LOG_INFO(HAL, "device %s opened (fd=%d)", device->get_name().c_str(), device->get_fd());

    // 创建 IPv4 层
    IPv4Layer ip_layer(*device);
    uint32_t local_ip = ip_from_string("192.168.100.2");
    ip_layer.set_local_ip(local_ip);

    // 创建 ICMP 处理器并注册到 IPv4 层
    ICMPHandler icmp_handler(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &icmp_handler);

    // 创建 UDP 层并注册到 IPv4 层
    UDPLayer udp_layer(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::UDP), &udp_layer);

    // 绑定一个测试端口 (7 = Echo 服务)
    uint16_t echo_port = udp_layer.bind(7, [&udp_layer](uint32_t src_ip, uint16_t src_port,
                                                         const uint8_t *data, size_t len) {
        LOG_INFO(UDP, "echo: received %zu bytes from %s:%u",
                 len, ip_to_string(src_ip).c_str(), src_port);
        LOG_HEXDUMP(UDP, DEBUG, data, len);
        udp_layer.sendto(src_ip, src_port, 7, data, len);
        LOG_DEBUG(UDP, "echo: sent %zu bytes back", len);
    });

    if (echo_port == 0) {
        LOG_FATAL(UDP, "failed to bind echo port");
        return EXIT_FAILURE;
    }

    LOG_INFO(APP, "local IP: %s", ip_to_string(local_ip).c_str());
    LOG_INFO(UDP, "echo service on port %u", echo_port);

    // 配置提示 (这些始终输出到stdout)
    std::printf("\nConfigure interface in another terminal:\n");
    std::printf("  sudo ifconfig %s 192.168.100.1 192.168.100.2 up\n\n",
        device->get_name().c_str());
    std::printf("Test with:\n");
    std::printf("  ping 192.168.100.2\n");
    std::printf("  echo 'Hello' | nc -u 192.168.100.2 7\n");
    std::printf("  nc -u 192.168.100.2 7        # interactive echo\n");
    std::printf("  nc -u 192.168.100.2 12345    # port unreachable\n\n");

    // 主循环
    uint8_t buf[2048];
    int pkt_count = 0;

    while (running) {
        ssize_t n = device->recv(buf, sizeof(buf), 1000);

        if (n > 0) {
            pkt_count++;

            auto pkt = IPv4Parser::parse(buf, n);
            if (pkt) {
                LOG_DEBUG(IPv4, "[#%d] %s -> %s, proto=%u, len=%u",
                    pkt_count,
                    ip_to_string(pkt->src_addr).c_str(),
                    ip_to_string(pkt->dst_addr).c_str(),
                    pkt->protocol,
                    pkt->total_length);

                LOG_HEXDUMP(IPv4, TRACE, pkt->payload,
                    pkt->payload_length > 64 ? 64 : pkt->payload_length);

                ip_layer.on_receive(buf, n);
            } else {
                LOG_WARN(IPv4, "[#%d] invalid packet (%zd bytes)", pkt_count, n);
            }
        } else if (n < 0) {
            LOG_ERROR(HAL, "recv error: %s", std::strerror(errno));
            break;
        }
        // n == 0 是超时，继续循环
    }

    LOG_INFO(APP, "shutting down, total packets: %d", pkt_count);
    device->close();

    return EXIT_SUCCESS;
}
