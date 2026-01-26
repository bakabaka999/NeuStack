/**
 * @file ip_addr.cpp
 * @brief IP address utility functions
 */

#include "neustack/common/ip_addr.hpp"

#include <cstdio>

uint32_t ip_from_string(const char* str) {
    uint32_t a, b, c, d;
    if (std::sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return 0;
    }
    return (a << 24) | (b << 16) | (c << 8) | d;
}

std::string ip_to_string(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        (addr >> 24) & 0xFF,
        (addr >> 16) & 0xFF,
        (addr >> 8) & 0xFF,
        addr & 0xFF);
    return buf;
}
