#ifndef NEUSTACK_COMMON_CHECKSUM_HPP
#define NEUSTACK_COMMON_CHECKSUM_HPP

#include <cstdint>
#include <cstddef>

namespace neustack {

/**
 * @brief 计算 Internet 校验和 (RFC 1071)
 * @param data 数据指针
 * @param len 数据长度 (字节)
 * @return 校验和 (网络字节序)
 */
uint16_t compute_checksum(const void *data, size_t len);

/**
 * @brief 验证校验和是否正确
 * @return true 如果校验和正确
 */
bool verify_checksum(const void *data, size_t len);

} // namespace neustack

#endif
