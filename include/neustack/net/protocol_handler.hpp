#ifndef NEUSTACK_NET_PROTOCOL_HANDLER_HPP
#define NEUSTACK_NET_PROTOCOL_HANDLER_HPP

namespace neustack {

// Forward declaration to avoid circular dependency
struct IPv4Packet;

/**
 * @brief Interface for protocol handlers (ICMP, TCP, UDP, etc.)
 */
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    /**
     * @brief Handle an incoming IPv4 packet
     * @param pkt Parsed IPv4 packet
     */
    virtual void handle(const IPv4Packet& pkt) = 0;
};

} // namespace neustack

#endif // NEUSTACK_NET_PROTOCOL_HANDLER_HPP
