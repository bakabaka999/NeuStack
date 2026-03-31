// tests/unit/hal/test_umem.cpp
// 测试 umem.hpp: UmemFrameAllocator 分配/释放逻辑, Umem 创建和地址转换

#include <catch2/catch_test_macros.hpp>
#include "neustack/hal/umem.hpp"
#include <set>
#include <algorithm>

using namespace neustack;

// ============================================================================
// UmemFrameAllocator 基础测试
// ============================================================================

TEST_CASE("UmemFrameAllocator construction", "[hal][umem]") {
    UmemFrameAllocator alloc(8, 4096);

    CHECK(alloc.available() == 8);
    CHECK(alloc.frame_size() == 4096);
}

TEST_CASE("UmemFrameAllocator single alloc/free", "[hal][umem]") {
    UmemFrameAllocator alloc(4, 4096);

    SECTION("Alloc returns valid address") {
        uint64_t addr = alloc.alloc();
        CHECK(addr != UmemFrameAllocator::INVALID_ADDR);
        CHECK(alloc.available() == 3);
    }

    SECTION("Free restores availability") {
        uint64_t addr = alloc.alloc();
        alloc.free(addr);
        CHECK(alloc.available() == 4);
    }

    SECTION("Alloc all then exhaust") {
        for (int i = 0; i < 4; ++i) {
            CHECK(alloc.alloc() != UmemFrameAllocator::INVALID_ADDR);
        }
        CHECK(alloc.available() == 0);
        CHECK(alloc.alloc() == UmemFrameAllocator::INVALID_ADDR);
    }
}

TEST_CASE("UmemFrameAllocator addresses are frame-aligned", "[hal][umem]") {
    constexpr uint32_t FRAME_SIZE = 4096;
    constexpr uint32_t COUNT = 16;
    UmemFrameAllocator alloc(COUNT, FRAME_SIZE);

    std::set<uint64_t> addrs;
    for (uint32_t i = 0; i < COUNT; ++i) {
        uint64_t addr = alloc.alloc();
        REQUIRE(addr != UmemFrameAllocator::INVALID_ADDR);
        // 每个地址都是 frame_size 对齐的
        CHECK(addr % FRAME_SIZE == 0);
        // 没有重复
        CHECK(addrs.insert(addr).second);
    }

    // 地址范围: [0, COUNT * FRAME_SIZE)
    for (auto a : addrs) {
        CHECK(a < static_cast<uint64_t>(COUNT) * FRAME_SIZE);
    }
}

TEST_CASE("UmemFrameAllocator non-4096 frame size", "[hal][umem]") {
    UmemFrameAllocator alloc(4, 2048);

    CHECK(alloc.frame_size() == 2048);

    uint64_t a0 = alloc.alloc();
    uint64_t a1 = alloc.alloc();
    CHECK(a0 != a1);
    // 地址间隔应该是 2048 的倍数
    CHECK(a0 % 2048 == 0);
    CHECK(a1 % 2048 == 0);
}

// ============================================================================
// UmemFrameAllocator 批量操作
// ============================================================================

TEST_CASE("UmemFrameAllocator batch alloc", "[hal][umem]") {
    UmemFrameAllocator alloc(8, 4096);

    SECTION("Batch alloc full") {
        uint64_t addrs[8];
        uint32_t n = alloc.alloc_batch(addrs, 8);
        CHECK(n == 8);
        CHECK(alloc.available() == 0);

        // 所有地址唯一
        std::set<uint64_t> s(addrs, addrs + 8);
        CHECK(s.size() == 8);
    }

    SECTION("Batch alloc partial") {
        uint64_t addrs[16];
        uint32_t n = alloc.alloc_batch(addrs, 16);
        CHECK(n == 8); // 只有 8 个
        CHECK(alloc.available() == 0);
    }

    SECTION("Batch alloc zero") {
        uint64_t addrs[1];
        uint32_t n = alloc.alloc_batch(addrs, 0);
        CHECK(n == 0);
        CHECK(alloc.available() == 8);
    }
}

TEST_CASE("UmemFrameAllocator batch free", "[hal][umem]") {
    UmemFrameAllocator alloc(8, 4096);

    uint64_t addrs[8];
    uint32_t n = alloc.alloc_batch(addrs, 8);
    REQUIRE(n == 8);
    CHECK(alloc.available() == 0);

    SECTION("Free all at once") {
        alloc.free_batch(addrs, 8);
        CHECK(alloc.available() == 8);
    }

    SECTION("Free in chunks") {
        alloc.free_batch(addrs, 4);
        CHECK(alloc.available() == 4);
        alloc.free_batch(addrs + 4, 4);
        CHECK(alloc.available() == 8);
    }
}

TEST_CASE("UmemFrameAllocator alloc-free-alloc reuse", "[hal][umem]") {
    UmemFrameAllocator alloc(2, 4096);

    uint64_t a1 = alloc.alloc();
    alloc.alloc(); // exhaust second frame
    CHECK(alloc.available() == 0);

    // 释放 a1，再分配应该拿回 a1（LIFO）
    alloc.free(a1);
    uint64_t a3 = alloc.alloc();
    CHECK(a3 == a1);
}

// ============================================================================
// Umem 创建和地址转换 (mmap 在容器里也能用)
// ============================================================================

TEST_CASE("Umem create and basic properties", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 16;
    cfg.frame_size = 4096;
    cfg.headroom = 0;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    CHECK(umem.buffer() != nullptr);
    CHECK(umem.buffer_size() == 16 * 4096);
    CHECK(umem.frame_size() == 4096);
    CHECK(umem.frame_count() == 16);
    CHECK(umem.available_frames() == 16);
}

TEST_CASE("Umem frame alloc and addr_to_ptr", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 4;
    cfg.frame_size = 4096;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    uint64_t addr = umem.alloc_frame();
    REQUIRE(addr != UmemFrameAllocator::INVALID_ADDR);

    uint8_t* ptr = umem.addr_to_ptr(addr);
    CHECK(ptr >= umem.buffer());
    CHECK(ptr < umem.buffer() + umem.buffer_size());

    // 写入数据验证指针有效
    ptr[0] = 0xAA;
    ptr[4095] = 0xBB;
    CHECK(ptr[0] == 0xAA);
    CHECK(ptr[4095] == 0xBB);
}

TEST_CASE("Umem ptr_to_addr roundtrip", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 4;
    cfg.frame_size = 4096;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    uint64_t addr = umem.alloc_frame();
    uint8_t* ptr = umem.addr_to_ptr(addr);
    uint64_t addr2 = umem.ptr_to_addr(ptr);
    CHECK(addr == addr2);
}

TEST_CASE("Umem alloc exhaustion and free", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 4;
    cfg.frame_size = 4096;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    uint64_t addrs[4];
    for (int i = 0; i < 4; ++i) {
        addrs[i] = umem.alloc_frame();
        CHECK(addrs[i] != UmemFrameAllocator::INVALID_ADDR);
    }
    CHECK(umem.available_frames() == 0);

    // 释放后可以再分配
    umem.free_frame(addrs[0]);
    CHECK(umem.available_frames() == 1);

    uint64_t reused = umem.alloc_frame();
    CHECK(reused == addrs[0]);
}

TEST_CASE("Umem batch alloc/free", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 8;
    cfg.frame_size = 4096;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    uint64_t addrs[8];
    uint32_t n = umem.alloc_frames(addrs, 8);
    CHECK(n == 8);
    CHECK(umem.available_frames() == 0);

    umem.free_frames(addrs, 8);
    CHECK(umem.available_frames() == 8);
}

TEST_CASE("Umem frames don't overlap", "[hal][umem]") {
    Umem::Config cfg;
    cfg.frame_count = 8;
    cfg.frame_size = 4096;

    Umem umem(cfg);
    REQUIRE(umem.create() == 0);

    uint64_t addrs[8];
    umem.alloc_frames(addrs, 8);

    // 每个 frame 写不同的标记，验证不互相覆盖
    for (int i = 0; i < 8; ++i) {
        uint8_t* ptr = umem.addr_to_ptr(addrs[i]);
        memset(ptr, static_cast<uint8_t>(i + 1), 4096);
    }

    for (int i = 0; i < 8; ++i) {
        uint8_t* ptr = umem.addr_to_ptr(addrs[i]);
        CHECK(ptr[0] == static_cast<uint8_t>(i + 1));
        CHECK(ptr[2048] == static_cast<uint8_t>(i + 1));
        CHECK(ptr[4095] == static_cast<uint8_t>(i + 1));
    }
}
