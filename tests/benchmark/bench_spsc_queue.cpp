/**
 * SPSC Queue Benchmark
 * * 测试 SPSCQueue 的单线程/多线程吞吐量及延迟
 */

#include "neustack/common/spsc_queue.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <cmath>

using namespace neustack;

// 使用高精度时钟
using Clock = std::chrono::high_resolution_clock;

// 队列大小配置 (必须是 2 的幂)
// 64KB * sizeof(int) = 256KB，适合 L2 Cache
constexpr size_t Q_SIZE = 65536; 

/**
 * 1. 单线程吞吐测试
 * 模拟单个线程内批量写入后批量读取，测试纯粹的内存操作开销
 */
void bench_single_thread() {
    constexpr int N = 1000000; // 总操作数
    // 使用 static 或 heap 分配以防栈溢出 (如果 Q_SIZE 很大)
    static SPSCQueue<int, Q_SIZE> queue; 

    auto start = Clock::now();

    int produced = 0;
    int consumed = 0;

    // 批量处理模式：写满再读空，模拟 Burst 流量
    while (consumed < N) {
        // 生产直到满
        while (produced < N && queue.try_push(produced)) {
            produced++;
        }
        
        // 消费直到空
        int val;
        while (queue.try_pop(val)) {
            consumed++;
        }
    }

    auto end = Clock::now();
    
    std::chrono::duration<double> diff = end - start;
    double mops = (N / diff.count()) / 1e6;

    std::cout << "SPSC single-thread: " << std::fixed << std::setprecision(2) << mops << " M ops/sec" << std::endl;
}

/**
 * 2. 双线程 Ping-Pong 吞吐测试
 * 典型的 SPSC 场景：一个生产者线程，一个消费者线程，全速运行
 */
void bench_dual_thread_throughput() {
    static SPSCQueue<int, Q_SIZE> queue;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_ops{0};

    // 消费者线程
    std::thread consumer([&]() {
        int val;
        while (running.load(std::memory_order_relaxed) || !queue.empty()) {
            if (queue.try_pop(val)) {
                total_ops.fetch_add(1, std::memory_order_relaxed);
            } else {
                // 队列空时让出 CPU，避免死循环空转过度抢占总线资源
                // 在极低延迟场景可能用 busy loop，这里为了测吞吐量适当 yield
                std::this_thread::yield(); 
            }
        }
    });

    // 生产者线程
    std::thread producer([&]() {
        int val = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (!queue.try_push(val)) {
                std::this_thread::yield();
            } else {
                val++;
            }
        }
    });

    // 运行 1 秒
    std::this_thread::sleep_for(std::chrono::seconds(1));
    running.store(false);

    producer.join();
    consumer.join();

    double mops = (total_ops.load() / 1.0) / 1e6;
    std::cout << "SPSC dual-thread:   " << std::fixed << std::setprecision(2) << mops << " M ops/sec" << std::endl;
}

/**
 * 3. 延迟测试
 * 测量数据从 push 到 pop 的端到端延迟
 */
void bench_latency() {
    using Timestamp = uint64_t;
    static SPSCQueue<Timestamp, Q_SIZE> queue;
    
    constexpr int SAMPLES = 200000;
    std::vector<double> latencies;
    latencies.reserve(SAMPLES);
    
    std::atomic<int> consumed_count{0};

    std::thread consumer([&]() {
        Timestamp sent_time;
        while (consumed_count < SAMPLES) {
            if (queue.try_pop(sent_time)) {
                auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch()).count();
                latencies.push_back(static_cast<double>(now - sent_time));
                consumed_count++;
            }
        }
    });

    std::thread producer([&]() {
        for (int i = 0; i < SAMPLES; ++i) {
            auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count();
            
            // 忙等待写入，确保记录的是写入时刻的时间
            while (!queue.try_push(now)) {
                std::this_thread::yield();
            }

            // 稍微控制一下生产速率，避免队列积压导致测量的是排队延迟而非传输延迟
            // 如果要测"满载延迟"，可以去掉这个 sleep
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });

    producer.join();
    consumer.join();

    // 计算分位值
    std::sort(latencies.begin(), latencies.end());
    double p50 = latencies[static_cast<size_t>(latencies.size() * 0.50)];
    double p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];

    std::cout << "SPSC latency p50:   " << static_cast<long long>(p50) << " ns, "
              << "p99: " << static_cast<long long>(p99) << " ns" << std::endl;
}

int main() {
    std::cout << "=== NeuStack SPSC Queue Benchmark ===" << std::endl;
    
    bench_single_thread();
    bench_dual_thread_throughput();
    bench_latency();

    return 0;
}