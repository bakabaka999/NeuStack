// tests/unit/hal/test_bpf_object.cpp
// 验证 BPF 编译产物 xdp_redirect.o 的正确性
// 仅在 NEUSTACK_ENABLE_AF_XDP 编译时运行

#include <catch2/catch_test_macros.hpp>

#ifdef NEUSTACK_ENABLE_AF_XDP

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <string>
#include <cstdio>

#ifndef NEUSTACK_BPF_OBJECT_DIR
#define NEUSTACK_BPF_OBJECT_DIR "."
#endif

static std::string bpf_object_path() {
    return std::string(NEUSTACK_BPF_OBJECT_DIR) + "/xdp_redirect.o";
}

TEST_CASE("xdp_redirect.o exists and is openable", "[hal][bpf]") {
    auto path = bpf_object_path();
    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);

    // 检查 ELF magic: 0x7f 'E' 'L' 'F'
    uint8_t magic[4];
    REQUIRE(fread(magic, 1, 4, f) == 4);
    fclose(f);

    CHECK(magic[0] == 0x7f);
    CHECK(magic[1] == 'E');
    CHECK(magic[2] == 'L');
    CHECK(magic[3] == 'F');
}

TEST_CASE("xdp_redirect.o can be opened by libbpf", "[hal][bpf]") {
    auto path = bpf_object_path();

    struct bpf_object* obj = bpf_object__open(path.c_str());
    REQUIRE(obj != nullptr);

    SECTION("contains xdp_sock_prog program") {
        struct bpf_program* prog = bpf_object__find_program_by_name(obj, "xdp_sock_prog");
        CHECK(prog != nullptr);
    }

    SECTION("contains xsks_map map") {
        struct bpf_map* map = bpf_object__find_map_by_name(obj, "xsks_map");
        CHECK(map != nullptr);
    }

    SECTION("program section is xdp") {
        struct bpf_program* prog = bpf_object__find_program_by_name(obj, "xdp_sock_prog");
        REQUIRE(prog != nullptr);
        const char* sec = bpf_program__section_name(prog);
        CHECK(std::string(sec) == "xdp");
    }

    bpf_object__close(obj);
}

TEST_CASE("xdp_redirect.o map properties", "[hal][bpf]") {
    auto path = bpf_object_path();
    struct bpf_object* obj = bpf_object__open(path.c_str());
    REQUIRE(obj != nullptr);

    struct bpf_map* map = bpf_object__find_map_by_name(obj, "xsks_map");
    REQUIRE(map != nullptr);

    CHECK(bpf_map__type(map) == BPF_MAP_TYPE_XSKMAP);
    CHECK(bpf_map__key_size(map) == sizeof(int));
    CHECK(bpf_map__value_size(map) == sizeof(int));
    CHECK(bpf_map__max_entries(map) == 64);

    bpf_object__close(obj);
}

#endif // NEUSTACK_ENABLE_AF_XDP
