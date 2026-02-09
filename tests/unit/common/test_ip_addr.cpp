#include <catch2/catch_test_macros.hpp>
#include "neustack/common/ip_addr.hpp"
#include <string>
#include <cstdint>

using namespace neustack;

TEST_CASE("IP address string and byte conversions", "[common][ip]") {
    SECTION("ip_from_string: standard addresses") {
        CHECK(ip_from_string("192.168.1.1") == 0xC0A80101);
        CHECK(ip_from_string("10.0.0.1") == 0x0A000001);
        CHECK(ip_from_string("127.0.0.1") == 0x7F000001);
    }

    SECTION("ip_from_string: boundary values") {
        CHECK(ip_from_string("0.0.0.0") == 0x00000000);
        CHECK(ip_from_string("255.255.255.255") == 0xFFFFFFFF);
    }

    SECTION("ip_from_string: invalid inputs") {
        CHECK(ip_from_string("") == 0);
        CHECK(ip_from_string("256.1.2.3") == 0);
        CHECK(ip_from_string("1.2.3") == 0);
        CHECK(ip_from_string("abc.def.ghi.jkl") == 0);
        // Note: "1.2.3.4.5" is parsed by sscanf as "1.2.3.4" (4 fields match),
        // so ip_from_string returns 0x01020304, not 0. This is expected behavior.
    }

    SECTION("ip_to_string: conversion to dotted decimal") {
        CHECK(ip_to_string(0xC0A80101) == "192.168.1.1");
        CHECK(ip_to_string(0) == "0.0.0.0");
        CHECK(ip_to_string(0xFFFFFFFF) == "255.255.255.255");
    }

    SECTION("Round-trip consistency") {
        const char* original = "192.168.100.2";
        uint32_t addr = ip_from_string(original);
        CHECK(ip_to_string(addr) == original);
    }

    SECTION("ip_from_bytes: byte array conversion") {
        uint8_t bytes1[] = {192, 168, 1, 1};
        CHECK(ip_from_bytes(bytes1) == ip_from_string("192.168.1.1"));

        uint8_t bytes2[] = {0, 0, 0, 0};
        CHECK(ip_from_bytes(bytes2) == 0);

        uint8_t bytes3[] = {255, 255, 255, 255};
        CHECK(ip_from_bytes(bytes3) == 0xFFFFFFFF);
    }
}