/**
 * TCP Throughput Benchmark
 * * 测试 TCPParser 解析、TCPBuilder 构建以及 Checksum 计算的吞吐量
 */

#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/net/ipv4.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <iomanip>
#include <cstring>

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

// 防止编译器优化
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

static constexpr int ITERATIONS = 1000000;

/**
 * 1. TCP 解析吞吐量测试
 * 模拟从网卡接收到 IP 包后，TCPParser 解析头部的过程
 */
void bench_tcp_parse() {
    // 1. 预先构造一个合法的 TCP SYN 包
    uint8_t buffer[1500];
    TCPBuilder builder;
    builder.set_src_port(12345)
           .set_dst_port(80)
           .set_seq(1001)
           .set_ack(0)
           .set_flags(0x02) // SYN
           .set_window(65535);
    
    ssize_t len = builder.build(buffer, sizeof(buffer));
    
    // 填充校验和 (解析器通常会校验)
    uint32_t src_ip = 0x0A000001;
    uint32_t dst_ip = 0x0A000002;
    TCPBuilder::fill_checksum(buffer, len, src_ip, dst_ip);

    // 构造 IPv4 伪包
    IPv4Packet ip_pkt;
    ip_pkt.src_addr = src_ip;
    ip_pkt.dst_addr = dst_ip;
    ip_pkt.protocol = 6; // TCP
    ip_pkt.payload = buffer;
    ip_pkt.payload_length = len;

    // 2. 开始测试
    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        auto seg = TCPParser::parse(ip_pkt);
        // 防止优化，检查一个字段
        if (seg) {
            do_not_optimize(seg->src_port);
        }
    }

    auto end = Clock::now();

    // 3. 计算指标
    std::chrono::duration<double> diff = end - start;
    double seconds = diff.count();
    double mpps = (ITERATIONS / seconds) / 1e6;
    
    // 吞吐量 Gbps = (包大小 * 8 * 包数) / 时间
    // 包大小：这里指 TCP 头部大小 (20 bytes) + 潜在的 IP 头(20 bytes) + Eth 头(14 bytes)
    // 但通常只计算处理的数据量。这里只计算 TCP segment 本身的大小。
    double gbps = (len * 8.0 * ITERATIONS / seconds) / 1e9;

    std::cout << "TCP parse:     " << std::fixed << std::setprecision(2) 
              << mpps << " Mpps (" << gbps << " Gbps)" << std::endl;
}

/**
 * 2. TCP 构建吞吐量测试
 * 模拟发送端根据 socket 信息构造 TCP 报文的过程
 */
void bench_tcp_build() {
    uint8_t buffer[1500];
    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        // 每次循环都重新设置字段，模拟真实场景
        TCPBuilder builder;
        builder.set_src_port(80)
               .set_dst_port(12345)
               .set_seq(i)         // 变化的 Seq
               .set_ack(i + 1)
               .set_flags(0x10)    // ACK
               .set_window(32768);
        
        ssize_t len = builder.build(buffer, sizeof(buffer));
        do_not_optimize(len);
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double mpps = (ITERATIONS / diff.count()) / 1e6;

    std::cout << "TCP build:     " << std::fixed << std::setprecision(2) << mpps << " Mpps" << std::endl;
}

/**
 * 3. TCP Checksum 计算吞吐量测试
 * 测试 fill_checksum 的性能 (这是发送路径上最耗时的操作之一)
 */
void bench_tcp_checksum() {
    // 准备数据
    uint8_t buffer[1500];
    TCPBuilder builder;
    builder.set_src_port(443).set_dst_port(56789).set_seq(100).set_ack(200).set_flags(0x18);
    // 添加一些 payload 增加计算负载 (模拟真实数据包)
    uint8_t payload[1000]; 
    std::memset(payload, 0xAB, sizeof(payload));
    builder.set_payload(payload, sizeof(payload));

    ssize_t len = builder.build(buffer, sizeof(buffer));
    uint32_t src = 0x01010101;
    uint32_t dst = 0x02020202;

    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        TCPBuilder::fill_checksum(buffer, len, src, dst);
        // 简单修改内存防止缓存完全命中优化过度 (可选)
        buffer[0]++; 
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double mpps = (ITERATIONS / diff.count()) / 1e6;
    double gbps = (len * 8.0 * ITERATIONS / diff.count()) / 1e9;

    std::cout << "TCP checksum:  " << std::fixed << std::setprecision(2) 
              << mpps << " Mpps (" << gbps << " Gbps)" << std::endl;
}

int main() {
    std::cout << "=== NeuStack TCP Throughput Benchmark ===" << std::endl;
    std::cout << "Iterations: " << ITERATIONS << std::endl;

    bench_tcp_parse();
    bench_tcp_build();
    bench_tcp_checksum();

    return 0;
}