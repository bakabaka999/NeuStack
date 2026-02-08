/**
 * Checksum Performance Benchmark
 * * 测试 compute_checksum 和 verify_checksum 的吞吐量
 */

#include "neustack/common/checksum.hpp"
#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <numeric>

using namespace neustack;

// 防止编译器优化掉计算过程
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

// 性能测试核心函数
void run_benchmark(const char* label, size_t size, int iterations, bool is_verify) {
    // 1. 准备数据
    // 使用 0xAA 填充，确保不是全零
    std::vector<uint8_t> buffer(size, 0xAA);

    if (is_verify && size >= 2) {
        // 如果是验证测试，先计算正确的 checksum 并填入最后 2 字节
        // 这样 verify_checksum 会返回 true (模拟真实场景)
        // 假设最后两个字节是 Checksum 字段位置
        buffer[size - 1] = 0;
        buffer[size - 2] = 0;
        
        uint16_t sum = compute_checksum(buffer.data(), size);
        std::memcpy(&buffer[size - 2], &sum, sizeof(sum));
    }

    // 2. 预热 (Warm up)
    if (is_verify) {
        do_not_optimize(verify_checksum(buffer.data(), size));
    } else {
        do_not_optimize(compute_checksum(buffer.data(), size));
    }

    // 3. 开始计时
    auto start = std::chrono::high_resolution_clock::now();

    if (is_verify) {
        for (int i = 0; i < iterations; ++i) {
            bool res = verify_checksum(buffer.data(), size);
            do_not_optimize(res);
        }
    } else {
        for (int i = 0; i < iterations; ++i) {
            uint16_t res = compute_checksum(buffer.data(), size);
            do_not_optimize(res);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();

    // 4. 计算吞吐量
    std::chrono::duration<double> diff = end - start;
    double seconds = diff.count();
    
    // 总比特数 = 字节数 * 8 * 次数
    double total_bits = static_cast<double>(size) * 8.0 * iterations;
    double gbps = (total_bits / seconds) / 1e9; // Gbps

    // 5. 输出结果
    // 格式: Checksum 64B:    X.XX Gbps (N iterations)
    std::cout << std::left << std::setw(18) << (std::string(label) + " " + std::to_string(size) + "B:")
              << std::fixed << std::setprecision(2) << std::right << std::setw(6) << gbps << " Gbps "
              << "(" << iterations << " iterations)" << std::endl;
}

int main() {
    std::cout << "=== NeuStack Checksum Benchmark ===" << std::endl;
    std::cout << "[Compute Checksum]" << std::endl;

    const int iterations = 100000;
    std::vector<size_t> sizes = {64, 512, 1500, 9000, 65535};

    // 1. compute_checksum 吞吐量
    for (size_t size : sizes) {
        run_benchmark("Compute", size, iterations, false);
    }

    std::cout << "\n[Verify Checksum]" << std::endl;

    // 2. verify_checksum 吞吐量
    for (size_t size : sizes) {
        run_benchmark("Verify ", size, iterations, true);
    }

    return 0;
}