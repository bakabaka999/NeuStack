#ifndef NEUSTACK_TRANSPORT_TCP_STATE_HPP
#define NEUSTACK_TRANSPORT_TCP_STATE_HPP

#include <cstdint>

// ========================================================================
// TCP 状态
// ========================================================================

enum class TCPState : uint8_t {
    CLOSED = 0,
    LISTEN,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    CLOSING,
    LAST_ACK,
    TIME_WAIT
};

// 状态名称（用于调试）
inline const char* tcp_state_name(TCPState state) {
    switch (state) {
        case TCPState::CLOSED:      return "CLOSED";
        case TCPState::LISTEN:      return "LISTEN";
        case TCPState::SYN_SENT:    return "SYN_SENT";
        case TCPState::SYN_RCVD:    return "SYN_RCVD";
        case TCPState::ESTABLISHED: return "ESTABLISHED";
        case TCPState::FIN_WAIT_1:  return "FIN_WAIT_1";
        case TCPState::FIN_WAIT_2:  return "FIN_WAIT_2";
        case TCPState::CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCPState::CLOSING:     return "CLOSING";
        case TCPState::LAST_ACK:    return "LAST_ACK";
        case TCPState::TIME_WAIT:   return "TIME_WAIT";
        default:                    return "UNKNOWN";
    }
}

#endif // NEUSTACK_TRANSPORT_TCP_STATE_HPP