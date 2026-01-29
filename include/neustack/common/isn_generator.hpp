#ifndef NEUSTACK_COMMON_ISN_GENERATOR_HPP
#define NEUSTACK_COMMON_ISN_GENERATOR_HPP

#include <cstdint>
#include <chrono>
#include <random>

namespace neustack {

/**
 * TCP 初始序列号生成器 (RFC 6528)
 *
 * ISN = M + F(local_ip, local_port, remote_ip, remote_port, secret)
 *
 * 其中:
 * - M: 4 微秒计时器，防止同一连接快速重用时序列号重叠
 * - F: SipHash-2-4，基于四元组和密钥生成，防止序列号预测攻击
 *
 * 安全性:
 * - 密钥在进程启动时随机生成
 * - 不同四元组产生不同的哈希值
 * - 攻击者无法预测其他连接的序列号
 */
class ISNGenerator {
public:
    // 生成初始序列号
    static uint32_t generate(uint32_t local_ip, uint16_t local_port,
                             uint32_t remote_ip, uint16_t remote_port) {
        // 时间组件：每 4 微秒递增
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        uint32_t M = static_cast<uint32_t>(us / 4);

        // 哈希组件：基于四元组 + 密钥
        uint32_t F = siphash(local_ip, local_port, remote_ip, remote_port);

        return M + F;
    }

    // 简化版本（向后兼容）
    static uint32_t generate() {
        return generate(0, 0, 0, 0);
    }

private:
    // 获取静态密钥（进程启动时随机生成）
    static std::pair<uint64_t, uint64_t> get_secret_key() {
        static const uint64_t k0 = 0x736f6d6570736575ULL ^ std::random_device{}();
        static const uint64_t k1 = 0x646f72616e646f6dULL ^ std::random_device{}();
        return {k0, k1};
    }

    // SipHash-2-4 实现
    // 参考: https://github.com/veorq/SipHash
    static uint32_t siphash(uint32_t lip, uint16_t lp,
                            uint32_t rip, uint16_t rp) {
        auto [k0, k1] = get_secret_key();

        // 初始化状态
        uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
        uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
        uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
        uint64_t v3 = k1 ^ 0x7465646279746573ULL;

        // 输入数据：四元组打包
        uint64_t m0 = (static_cast<uint64_t>(lip) << 32) |
                      (static_cast<uint64_t>(lp) << 16) | rp;
        uint64_t m1 = static_cast<uint64_t>(rip);

        // 压缩阶段
        v3 ^= m0;
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);
        v0 ^= m0;

        v3 ^= m1;
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);
        v0 ^= m1;

        // 终结阶段
        v2 ^= 0xff;
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);

        return static_cast<uint32_t>(v0 ^ v1 ^ v2 ^ v3);
    }

    // SipHash 轮函数
    static void sipround(uint64_t& v0, uint64_t& v1,
                         uint64_t& v2, uint64_t& v3) {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);

        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;

        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;

        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    }

    // 循环左移
    static uint64_t rotl(uint64_t x, int n) {
        return (x << n) | (x >> (64 - n));
    }
};

} // namespace neustack

#endif // NEUSTACK_COMMON_ISN_GENERATOR_HPP
