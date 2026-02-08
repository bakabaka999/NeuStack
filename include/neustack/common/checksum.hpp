#ifndef NEUSTACK_COMMON_CHECKSUM_HPP
#define NEUSTACK_COMMON_CHECKSUM_HPP

#include <cstdint>
#include <cstddef>

namespace neustack {

/**
 * @brief 累加数据到校验和（不折叠、不取反）
 *
 * 可多次调用以分段累加，最终由 checksum_finalize 完成。
 * @param sum 当前累加值（首次传 0）
 * @param data 数据指针
 * @param len 数据长度
 * @return 更新后的累加值
 */
uint32_t checksum_accumulate(uint32_t sum, const void *data, size_t len);

/**
 * @brief 折叠进位 + 取反，返回最终校验和
 * @param sum 累加值
 * @return 校验和（网络字节序）
 */
uint16_t checksum_finalize(uint32_t sum);

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
