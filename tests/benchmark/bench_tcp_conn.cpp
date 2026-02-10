/**
 * TCP Connection & Throughput Benchmark
 * * 测量:
 * 1. 连接建立速率 (Connections/sec)
 * 2. 端到端数据传输吞吐量 (MB/s)
 * * 使用 MockDevice 在内存中运行两个完整的 TCP/IP 协议栈。
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/transport/stream.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <vector>
#include <queue>
#include <string>

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// MockDevice — 内存网络设备 (复用自 test_tcp_handshake.cpp)
// ============================================================================

class MockDevice : public NetDevice {
public:
    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "bench_dev"; }

    ssize_t send(const uint8_t* data, size_t len) override {
        _outbox.push(std::vector<uint8_t>(data, data + len));
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t* buf, size_t len, int /*timeout_ms*/) override {
        if (_inbox.empty()) return 0;
        auto& pkt = _inbox.front();
        size_t n = std::min(len, pkt.size());
        std::memcpy(buf, pkt.data(), n);
        _inbox.pop();
        return static_cast<ssize_t>(n);
    }

    bool has_output() const { return !_outbox.empty(); }

    std::vector<uint8_t> pop_output() {
        auto pkt = std::move(_outbox.front());
        _outbox.pop();
        return pkt;
    }

private:
    std::queue<std::vector<uint8_t>> _outbox;
    std::queue<std::vector<uint8_t>> _inbox;
};

// ============================================================================
// 辅助函数
// ============================================================================

// 在两端之间转发数据包
static void transfer_packets(MockDevice& from, IPv4Layer& to_ip) {
    // 限制单次转发数量，模拟网络处理
    int limit = 50; 
    while (from.has_output() && limit-- > 0) {
        auto pkt = from.pop_output();
        to_ip.on_receive(pkt.data(), pkt.size());
    }
}

// 防止编译器优化
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

// ============================================================================
// Benchmark 1: 连接建立速率
// ============================================================================

void bench_connection_rate() {
    MockDevice server_dev, client_dev;
    
    IPv4Layer server_ip(server_dev);
    IPv4Layer client_ip(client_dev);
    
    uint32_t srv_addr = ip_from_string("192.168.100.1");
    uint32_t cli_addr = ip_from_string("192.168.100.2");
    
    server_ip.set_local_ip(srv_addr);
    client_ip.set_local_ip(cli_addr);
    
    TCPLayer server_tcp(server_ip, srv_addr);
    TCPLayer client_tcp(client_ip, cli_addr);
    
    server_ip.register_handler(6, &server_tcp);
    client_ip.register_handler(6, &client_tcp);

    // 服务端监听
    server_tcp.listen(8080, [](IStreamConnection* conn) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection*, const uint8_t*, size_t) {},
            .on_close = [](IStreamConnection* c) { 
                // 服务端被动接受关闭
            }
        };
    });

    const int iterations = 1000;
    int completed_connections = 0;

    auto start = Clock::now();

    for (int i = 0; i < iterations; ++i) {
        bool connected = false;
        
        // 客户端发起连接 (使用不同端口避免 TIME_WAIT 状态下的四元组冲突)
        // 端口范围: 10000 ~ 11000
        client_tcp.connect(srv_addr, 8080, 10000 + i,
            [&](IStreamConnection* conn, int err) {
                if (err == 0) {
                    connected = true;
                    // 连接成功后立即关闭，释放资源
                    conn->close();
                }
            },
            [](IStreamConnection*, const uint8_t*, size_t) {},
            [](IStreamConnection*) {}
        );

        // 驱动事件循环直到连接建立
        // 通常需要 3-4 个 tick (SYN -> SYN+ACK -> ACK)
        int ticks = 0;
        while (!connected && ticks++ < 100) {
            transfer_packets(client_dev, server_ip);
            transfer_packets(server_dev, client_ip);
            
            // 定时器驱动重传和状态机
            server_tcp.on_timer();
            client_tcp.on_timer();
        }

        if (connected) {
            completed_connections++;
        }
    }

    auto end = Clock::now();
    
    std::chrono::duration<double> diff = end - start;
    double rate = completed_connections / diff.count();

    std::cout << "TCP conn rate:   " 
              << std::fixed << std::setprecision(2) << rate << " conn/s "
              << "(" << completed_connections << " connections)" << std::endl;
}

// ============================================================================
// Benchmark 2: 端到端数据传输吞吐量
// ============================================================================

void bench_data_throughput() {
    MockDevice server_dev, client_dev;
    IPv4Layer server_ip(server_dev), client_ip(client_dev);
    
    uint32_t srv_addr = ip_from_string("192.168.100.1");
    uint32_t cli_addr = ip_from_string("192.168.100.2");
    
    server_ip.set_local_ip(srv_addr);
    client_ip.set_local_ip(cli_addr);
    
    TCPLayer server_tcp(server_ip, srv_addr);
    TCPLayer client_tcp(client_ip, cli_addr);
    
    server_ip.register_handler(6, &server_tcp);
    client_ip.register_handler(6, &client_tcp);

    // 优化配置：增大窗口以提升吞吐
    TCPOptions opts = TCPOptions::high_throughput();
    opts.recv_buffer_size = 256 * 1024;
    opts.send_buffer_size = 256 * 1024;
    server_tcp.set_default_options(opts);
    client_tcp.set_default_options(opts);

    size_t total_received = 0;

    // 服务端：统计接收字节数
    server_tcp.listen(9090, [&](IStreamConnection* conn) -> StreamCallbacks {
        return {
            .on_receive = [&](IStreamConnection*, const uint8_t* data, size_t len) {
                total_received += len;
                do_not_optimize(data[0]); // 触碰数据防止完全优化
            },
            .on_close = [](IStreamConnection*) {}
        };
    });

    // 建立连接
    IStreamConnection* cli_conn = nullptr;
    bool connected = false;

    client_tcp.connect(srv_addr, 9090, 0,
        [&](IStreamConnection* conn, int err) {
            if (err == 0) {
                connected = true;
                cli_conn = conn;
            }
        },
        [](IStreamConnection*, const uint8_t*, size_t) {},
        [](IStreamConnection*) {}
    );

    // 握手循环
    while (!connected) {
        transfer_packets(client_dev, server_ip);
        transfer_packets(server_dev, client_ip);
        server_tcp.on_timer();
        client_tcp.on_timer();
    }

    // 准备数据
    const size_t CHUNK_SIZE = 1460; // 1 MSS
    std::vector<uint8_t> payload(CHUNK_SIZE, 'X');
    const size_t TOTAL_BYTES = 10 * 1024 * 1024; // 发送 10 MB

    auto start = Clock::now();

    size_t sent_bytes = 0;
    
    // 发送循环
    while (total_received < TOTAL_BYTES) {
        // 1. 尝试填满发送缓冲区
        // 发送直到发完目标量
        if (sent_bytes < TOTAL_BYTES) {
            // 简单流控：如果还可以发，就发
            // 这里的 send 写入的是 socket buffer
            // 实际测试中，我们猛发一阵，然后驱动网络
            for (int i = 0; i < 10; ++i) { // 每次尝试发 10 个包
                if (sent_bytes >= TOTAL_BYTES) break;
                ssize_t n = cli_conn->send(payload.data(), payload.size());
                if (n > 0) {
                    sent_bytes += n;
                } else {
                    break; // 缓冲区满
                }
            }
        }

        // 2. 驱动网络层 (数据搬运)
        transfer_packets(client_dev, server_ip);
        transfer_packets(server_dev, client_ip);

        // 3. 驱动协议栈 (ACK, 重传, 窗口更新)
        // 这里的频率决定了模拟时间的流逝速度
        server_tcp.on_timer();
        client_tcp.on_timer();
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double mbs = (total_received / (1024.0 * 1024.0)) / diff.count();

    std::cout << "TCP throughput:  " 
              << std::fixed << std::setprecision(2) << mbs << " MB/s "
              << "(Payload: " << (TOTAL_BYTES / 1024 / 1024) << " MB)" << std::endl;

    // 清理
    if (cli_conn) cli_conn->close();
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // 禁用日志以避免 I/O 成为瓶颈
    Logger::instance().set_level(LogLevel::ERROR);
    
    std::cout << "=== NeuStack TCP Connection Benchmark ===" << std::endl;

    bench_connection_rate();
    bench_data_throughput();

    return 0;
}