#ifndef NEUSTACK_COMMON_IP_ADDR_HPP
#define NEUSTACK_COMMON_IP_ADDR_HPP

#include <string>
#include <cstdint>

namespace neustack {

/**
 * @brief 点分十进制字符串转 32 位整数
 * @param str "192.168.1.1"
 * @return 主机字节序的 32 位地址，失败返回 0
 */
uint32_t ip_from_string(const char *str);

/**
 * @brief 32 位整数转点分十进制字符串
 * @param addr 主机字节序的 32 位地址
 * @return "192.168.1.1"
 */
std::string ip_to_string(uint32_t addr);

/**
 * @brief 从 4 字节数组构造地址 (网络字节序)
 */
inline uint32_t ip_from_bytes(const uint8_t *bytes)
{
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

} // namespace neustack

#endif
