/**
 * @file main.cpp
 * @brief NeuStack - User-space TCP/IP Stack
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

using namespace neustack;

static volatile bool running = true;

void signal_handler(int) {
    running = false;
}

// ============================================================================
// Main
// ============================================================================

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

    // 创建 TCP 层并注册到 IPv4 层
    TCPLayer tcp_layer(ip_layer, local_ip);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &tcp_layer);

    // 设置 TCP 默认选项（交互式应用：禁用 Nagle，启用延迟 ACK）
    tcp_layer.set_default_options(TCPOptions::interactive());

    // ─── UDP Echo 服务 (端口 7) ───
    uint16_t echo_port = udp_layer.bind(7, [&udp_layer](uint32_t src_ip, uint16_t src_port,
                                                         const uint8_t *data, size_t len) {
        LOG_INFO(UDP, "echo: received %zu bytes from %s:%u",
                 len, ip_to_string(src_ip).c_str(), src_port);
        LOG_HEXDUMP(UDP, DEBUG, data, len);
        udp_layer.sendto(src_ip, src_port, 7, data, len);
        LOG_DEBUG(UDP, "echo: sent %zu bytes back", len);
    });

    if (echo_port == 0) {
        LOG_FATAL(UDP, "failed to bind UDP echo port");
        return EXIT_FAILURE;
    }

    // ─── TCP Echo 服务 (端口 7) ───
    int tcp_listen_result = tcp_layer.listen(7, [&tcp_layer](TCB* tcb) -> TCPCallbacks {
        LOG_INFO(TCP, "New TCP connection from %s:%u",
                 ip_to_string(tcb->t_tuple.remote_ip).c_str(),
                 tcb->t_tuple.remote_port);

        return TCPCallbacks{
            // on_receive: echo 收到的数据
            .on_receive = [&tcp_layer](TCB* tcb, const uint8_t* data, size_t len) {
                LOG_INFO(TCP, "echo: received %zu bytes, sending back", len);
                tcp_layer.send(tcb, data, len);
            },
            // on_close: 对方关闭连接，我们也关闭
            .on_close = [&tcp_layer](TCB* tcb) {
                LOG_INFO(TCP, "echo: peer closed, closing our side");
                tcp_layer.close(tcb);
            }
        };
    });

    if (tcp_listen_result < 0) {
        LOG_FATAL(TCP, "failed to listen on TCP port 7");
        return EXIT_FAILURE;
    }

    LOG_INFO(APP, "local IP: %s", ip_to_string(local_ip).c_str());
    LOG_INFO(UDP, "UDP echo service on port %u", echo_port);
    LOG_INFO(TCP, "TCP echo service on port 7");

    // 配置提示
    std::printf("\nConfigure interface in another terminal:\n");
    std::printf("  sudo ifconfig %s 192.168.100.1 192.168.100.2 up\n\n",
        device->get_name().c_str());
    std::printf("Test with:\n");
    std::printf("  ping 192.168.100.2\n");
    std::printf("  echo 'Hello' | nc -u 192.168.100.2 7    # UDP echo\n");
    std::printf("  nc 192.168.100.2 7                      # TCP echo (interactive)\n");
    std::printf("  nc -v 192.168.100.2 7                   # TCP with verbose\n\n");

    // 主循环
    uint8_t buf[2048];
    int pkt_count = 0;
    auto last_timer = std::chrono::steady_clock::now();
    constexpr auto TIMER_INTERVAL = std::chrono::milliseconds(100);

    while (running) {
        ssize_t n = device->recv(buf, sizeof(buf), 100);  // 100ms 超时

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
            if (errno == EINTR) {
                continue;  // 被信号中断，继续循环（running 会被设为 false）
            }
            LOG_ERROR(HAL, "recv error: %s", std::strerror(errno));
            break;
        }

        // 定时器处理
        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= TIMER_INTERVAL) {
            tcp_layer.on_timer();
            last_timer = now;
        }
    }

    LOG_INFO(APP, "shutting down, total packets: %d", pkt_count);
    device->close();

    return EXIT_SUCCESS;
}
