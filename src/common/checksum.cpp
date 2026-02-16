/**
 * @file checksum.cpp
 * @brief Internet checksum implementation (RFC 1071)
 */

#include "neustack/common/checksum.hpp"
#include "neustack/common/platform.hpp"

namespace neustack {

uint32_t checksum_accumulate(uint32_t sum, const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    while (len > 1) {
        sum += (ptr[0] << 8) | ptr[1];
        ptr += 2;
        len -= 2;
    }

    if (len == 1) {
        sum += ptr[0] << 8;
    }

    return sum;
}

uint16_t checksum_finalize(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return htons(static_cast<uint16_t>(~sum & 0xFFFF));
}

uint16_t compute_checksum(const void* data, size_t len) {
    return checksum_finalize(checksum_accumulate(0, data, len));
}

bool verify_checksum(const void* data, size_t len) {
    uint32_t sum = checksum_accumulate(0, data, len);

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (sum & 0xFFFF) == 0xFFFF;
}

} // namespace neustack
