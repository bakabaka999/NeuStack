/**
 * Memory Pool Benchmark
 * * 对比 FixedPool<T> 与标准 new/delete 的性能
 */

#include "neustack/common/memory_pool.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <memory>
#include <numeric>

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

// 防止编译器优化
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

// 模拟 TCB (64 字节)
struct MockTCB {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t state;
    uint8_t padding[64 - 13]; // 补齐到 64 字节

    MockTCB() : seq_num(0), ack_num(0), src_port(0), dst_port(0), state(0) {}
};

// ============================================================================
// 辅助函数：打印结果
// ============================================================================

void print_result(const char* label, double pool_val, double heap_val, const char* unit, bool is_rate) {
    std::cout << "[" << label << "]" << std::endl;
    std::cout << "  FixedPool:   " << std::fixed << std::setprecision(2) << pool_val << " " << unit << std::endl;
    std::cout << "  new/delete:  " << std::fixed << std::setprecision(2) << heap_val << " " << unit << std::endl;
    
    double speedup = is_rate ? (pool_val / heap_val) : (heap_val / pool_val);
    std::cout << "  Speedup:     " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
    std::cout << std::endl;
}

// ============================================================================
// Benchmark 1: 顺序 acquire/release
// ============================================================================

void bench_sequential() {
    constexpr int N = 1000000;
    static FixedPool<MockTCB, 10> pool; // 只需要很小的池即可，因为分配后立即释放

    // --- FixedPool ---
    auto start_pool = Clock::now();
    for (int i = 0; i < N; ++i) {
        auto* p = pool.acquire();
        do_not_optimize(p);
        pool.release(p);
    }
    auto end_pool = Clock::now();

    // --- new/delete ---
    auto start_heap = Clock::now();
    for (int i = 0; i < N; ++i) {
        auto* p = new MockTCB();
        do_not_optimize(p);
        delete p;
    }
    auto end_heap = Clock::now();

    double pool_sec = std::chrono::duration<double>(end_pool - start_pool).count();
    double heap_sec = std::chrono::duration<double>(end_heap - start_heap).count();

    print_result("Sequential acquire/release", 
                 (N / pool_sec) / 1e6, 
                 (N / heap_sec) / 1e6, 
                 "M ops/s", true);
}

// ============================================================================
// Benchmark 2: 批量分配再批量释放
// ============================================================================

void bench_batch() {
    constexpr int BATCH_SIZE = 10000;
    constexpr int ROUNDS = 100;
    
    // 需要足够大的池
    static FixedPool<MockTCB, BATCH_SIZE> pool;
    std::vector<MockTCB*> ptrs;
    ptrs.reserve(BATCH_SIZE);

    // --- FixedPool ---
    auto start_pool = Clock::now();
    for (int r = 0; r < ROUNDS; ++r) {
        // 批量分配
        for (int i = 0; i < BATCH_SIZE; ++i) {
            ptrs.push_back(pool.acquire());
        }
        // 批量释放
        for (auto* p : ptrs) {
            pool.release(p);
        }
        ptrs.clear();
    }
    auto end_pool = Clock::now();

    // 确保清理干净
    if (pool.in_use() != 0) {
        std::cerr << "Error: Pool leak detected in bench_batch" << std::endl;
        exit(1);
    }

    // --- new/delete ---
    auto start_heap = Clock::now();
    for (int r = 0; r < ROUNDS; ++r) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            ptrs.push_back(new MockTCB());
        }
        for (auto* p : ptrs) {
            delete p;
        }
        ptrs.clear();
    }
    auto end_heap = Clock::now();

    double pool_ms = std::chrono::duration<double, std::milli>(end_pool - start_pool).count();
    double heap_ms = std::chrono::duration<double, std::milli>(end_heap - start_heap).count();

    print_result("Batch 10000 acquire + 10000 release x100", 
                 pool_ms, 
                 heap_ms, 
                 "ms", false);
}

// ============================================================================
// Benchmark 3: 交替 acquire/release (模拟真实流量)
// ============================================================================

void bench_interleaved() {
    constexpr int N_OPS = 2000000;
    constexpr int POOL_SIZE = 20000;
    constexpr int TARGET_ACTIVE = POOL_SIZE / 2;

    static FixedPool<MockTCB, POOL_SIZE> pool;
    std::vector<MockTCB*> active;
    active.reserve(POOL_SIZE);

    // --- FixedPool ---
    auto start_pool = Clock::now();
    for (int i = 0; i < N_OPS; ++i) {
        if (active.size() < TARGET_ACTIVE) {
            active.push_back(pool.acquire());
        } else {
            pool.release(active.back());
            active.pop_back();
        }
    }
    // 清理剩余对象
    for (auto* p : active) pool.release(p);
    active.clear();
    auto end_pool = Clock::now();

    // --- new/delete ---
    auto start_heap = Clock::now();
    for (int i = 0; i < N_OPS; ++i) {
        if (active.size() < TARGET_ACTIVE) {
            active.push_back(new MockTCB());
        } else {
            delete active.back();
            active.pop_back();
        }
    }
    // 清理剩余对象
    for (auto* p : active) delete p;
    active.clear();
    auto end_heap = Clock::now();

    double pool_sec = std::chrono::duration<double>(end_pool - start_pool).count();
    double heap_sec = std::chrono::duration<double>(end_heap - start_heap).count();

    // Ops 包含分配和释放操作 (N_OPS + clean up ops)
    // 为了简单对比，我们只计算循环内的 ops
    print_result("Interleaved acquire/release", 
                 (N_OPS / pool_sec) / 1e6, 
                 (N_OPS / heap_sec) / 1e6, 
                 "M ops/s", true);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== NeuStack Memory Pool Benchmark ===" << std::endl << std::endl;

    bench_sequential();
    bench_batch();
    bench_interleaved();

    return 0;
}