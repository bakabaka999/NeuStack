#ifndef NEUSTACK_TRANSPORT_TCP_SEQ_HPP
#define NEUSTACK_TRANSPORT_TCP_SEQ_HPP

#include <cstdint>

// ========================================================================
// TCP 序列号比较 (处理回绕)
// ========================================================================

// 序列号使用模 2^32 位算数，比较时使用有符号差值

// a < b
inline bool seq_lt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

// a <= b
inline bool seq_le(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) <= 0;
}

// a > b
inline bool seq_gt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

// a >= b
inline bool seq_ge(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) >= 0;
}

// 检查 seq 是否在 [start, end) 范围内
inline bool seq_in_range(uint32_t seq, uint32_t start, uint32_t end) {
    return seq_ge(seq, start) && seq_lt(seq, end);
}

#endif // NEUSTACK_TRANSPORT_TCP_SEQ_HPP