/**
 * @file checksum.cpp
 * @brief Internet checksum implementation (RFC 1071)
 */

#include "neustack/common/checksum.hpp"

#include <arpa/inet.h>  // htons (cross-platform)

namespace neustack {

uint16_t compute_checksum(const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;

    // 按 16 位累加
    while (len > 1) {
        sum += (ptr[0] << 8) | ptr[1];  // 网络字节序 (大端)
        ptr += 2;
        len -= 2;
    }

    // 处理奇数字节 (结尾补 0)
    if (len == 1) {
        sum += ptr[0] << 8;
    }

    // 折叠进位
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // 取反码，返回网络字节序
    return htons(static_cast<uint16_t>(~sum & 0xFFFF));
}

bool verify_checksum(const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;

    // 按 16 位累加
    while (len > 1) {
        sum += (ptr[0] << 8) | ptr[1];
        ptr += 2;
        len -= 2;
    }

    // 处理奇数字节
    if (len == 1) {
        sum += ptr[0] << 8;
    }

    // 折叠进位
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // 如果校验和正确，结果应该是 0xFFFF
    return (sum & 0xFFFF) == 0xFFFF;
}

} // namespace neustack
