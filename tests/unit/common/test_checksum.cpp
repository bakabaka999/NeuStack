#include <catch2/catch_test_macros.hpp>
#include "neustack/common/checksum.hpp"
#include <vector>
#include <cstdint>
#include <arpa/inet.h>

using namespace neustack;

TEST_CASE("Checksum computation and verification", "[common][checksum]") {
    SECTION("All zeros data") {
        std::vector<uint8_t> data(10, 0);
        // RFC 1071: The checksum field is the 16 bit one's complement of the one's complement sum...
        // For all-zero data, the sum is 0, and the one's complement is 0xFFFF.
        CHECK(compute_checksum(data.data(), data.size()) == 0xFFFF);
    }

    SECTION("RFC 1071 example data") {
        uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
        // compute_checksum returns network byte order via htons()
        // One's complement sum = 0xddf2, ~sum = 0x220d, htons(0x220d) on LE = 0x0d22
        uint16_t result = compute_checksum(data, sizeof(data));
        CHECK(result == htons(0x220d));
    }

    SECTION("Odd length data handling") {
        // Data: {0x11, 0x22, 0x33} -> Should be treated as {0x1122, 0x3300}
        uint8_t data[] = {0x11, 0x22, 0x33};
        uint16_t expected = compute_checksum(data, 3);
        
        uint8_t padded_data[] = {0x11, 0x22, 0x33, 0x00};
        CHECK(expected == compute_checksum(padded_data, 4));
    }

    SECTION("Single byte data") {
        uint8_t data[] = {0xFF};
        // 0xFF00 → ~sum = 0x00FF → htons(0x00FF)
        CHECK(compute_checksum(data, 1) == htons(0x00FF));
    }

    SECTION("Empty data") {
        CHECK(compute_checksum(nullptr, 0) == 0xFFFF);
    }

    SECTION("Verification of valid data") {
        // Example data + its checksum (0x220d)
        // In real network headers, the checksum field itself is part of the verification.
        // We simulate a packet where the checksum field is filled with the result.
        struct FakePacket {
            uint8_t payload[8];
            uint16_t checksum;
        } packet = {
            {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7},
            0x0000 // Placeholder
        };

        // Standard way to verify: compute checksum of (data + checksum_field_set_to_0)
        // Then place that checksum into the field. verify_checksum(packet) should then be true.
        packet.checksum = compute_checksum(&packet, sizeof(packet));
        CHECK(verify_checksum(&packet, sizeof(packet)) == true);
    }

    SECTION("Verification of tampered data") {
        uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7, 0x22, 0x0d};
        
        SECTION("Original valid") {
            CHECK(verify_checksum(data, sizeof(data)) == true);
        }

        SECTION("Tampered payload") {
            data[0] ^= 0xFF; // Flip bits
            CHECK(verify_checksum(data, sizeof(data)) == false);
        }

        SECTION("Tampered checksum field") {
            data[9] += 1; // Corrupt the checksum byte
            CHECK(verify_checksum(data, sizeof(data)) == false);
        }
    }
}