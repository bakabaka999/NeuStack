#include "neustack/net/ipv4.hpp"
#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_segment.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace neustack;

namespace {

void touch_segment(const TCPSegment &segment) {
    (void)segment.src_addr;
    (void)segment.dst_addr;
    (void)segment.src_port;
    (void)segment.dst_port;
    (void)segment.seq_num;
    (void)segment.ack_num;
    (void)segment.flags;
    (void)segment.window;
    (void)segment.data_length;
    (void)segment.seg_len();
    (void)segment.seq_end();
}

void exercise(const uint8_t *data, size_t len, bool fix_checksum) {
    std::vector<uint8_t> segment(data, data + std::min<size_t>(len, 1460));
    if (segment.size() < sizeof(TCPHeader)) {
        segment.resize(sizeof(TCPHeader), 0);
    }

    auto *header = reinterpret_cast<TCPHeader *>(segment.data());
    header->data_offset = static_cast<uint8_t>((5u << 4) | (header->data_offset & 0x0F));

    constexpr uint32_t src_ip = 0x0A000001u;
    constexpr uint32_t dst_ip = 0x0A000002u;

    if (fix_checksum) {
        header->checksum = 0;
        TCPBuilder::fill_checksum(segment.data(), segment.size(), src_ip, dst_ip);
    }

    IPv4Packet packet{};
    packet.version = 4;
    packet.ihl = 5;
    packet.total_length = static_cast<uint16_t>(sizeof(IPv4Header) + segment.size());
    packet.ttl = 64;
    packet.protocol = static_cast<uint8_t>(IPProtocol::TCP);
    packet.src_addr = src_ip;
    packet.dst_addr = dst_ip;
    packet.payload = segment.data();
    packet.payload_length = segment.size();
    packet.raw_data = segment.data();
    packet.raw_length = segment.size();

    (void)TCPParser::verify_checksum(packet);
    auto parsed = TCPParser::parse(packet);
    if (parsed) {
        touch_segment(*parsed);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    exercise(data, len, false);
    exercise(data, len, true);
    return 0;
}
