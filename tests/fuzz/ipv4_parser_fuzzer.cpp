#include "neustack/common/checksum.hpp"
#include "neustack/net/ipv4.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace neustack;

namespace {

void touch_packet(const IPv4Packet &packet) {
    (void)packet.version;
    (void)packet.ihl;
    (void)packet.total_length;
    (void)packet.identification;
    (void)packet.ttl;
    (void)packet.protocol;
    (void)packet.src_addr;
    (void)packet.dst_addr;
    (void)packet.payload_length;
}

void exercise(const uint8_t *data, size_t len) {
    (void)IPv4Parser::validate(data, len);
    auto packet = IPv4Parser::parse(data, len);
    if (packet) {
        touch_packet(*packet);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    exercise(data, len);

    std::vector<uint8_t> normalized(data, data + std::min<size_t>(len, 1500));
    if (normalized.size() < sizeof(IPv4Header)) {
        normalized.resize(sizeof(IPv4Header), 0);
    }

    normalized[0] = 0x45;
    uint16_t total_length = htons(static_cast<uint16_t>(normalized.size()));
    std::memcpy(normalized.data() + 2, &total_length, sizeof(total_length));
    if (normalized[8] == 0) {
        normalized[8] = 64;
    }
    normalized[10] = 0;
    normalized[11] = 0;

    uint16_t checksum = compute_checksum(normalized.data(), sizeof(IPv4Header));
    std::memcpy(normalized.data() + 10, &checksum, sizeof(checksum));

    exercise(normalized.data(), normalized.size());
    return 0;
}
