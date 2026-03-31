// tests/unit/hal/test_xdp_ring.cpp
// 测试 xdp_ring.hpp: XdpRing 的 reserve/submit/peek/release 操作
// 用本地数组模拟 mmap 共享内存，不需要内核 XDP 支持

#include <catch2/catch_test_macros.hpp>
#include "neustack/hal/xdp_ring.hpp"
#include <cstdint>
#include <vector>

using namespace neustack;

// 用 uint64_t 模拟 Fill/Completion ring (和真实 UMEM 地址类型一致)
using AddrRing = XdpRing<uint64_t>;

// 模拟 xdp_desc 结构 (和 TX/RX ring 一致)
struct FakeDesc {
    uint64_t addr;
    uint32_t len;
    uint32_t options;
};
using DescRing = XdpRing<FakeDesc>;

// ============================================================================
// 辅助：创建一个本地模拟的 ring
// ============================================================================

struct LocalAddrRing {
    uint32_t producer = 0;
    uint32_t consumer = 0;
    uint32_t flags = 0;
    std::vector<uint64_t> descs;
    AddrRing ring;

    explicit LocalAddrRing(uint32_t size) : descs(size, 0) {
        ring.init(&producer, &consumer, descs.data(), size, &flags);
    }
};

struct LocalDescRing {
    uint32_t producer = 0;
    uint32_t consumer = 0;
    uint32_t flags = 0;
    std::vector<FakeDesc> descs;
    DescRing ring;

    explicit LocalDescRing(uint32_t size) : descs(size, FakeDesc{}) {
        ring.init(&producer, &consumer, descs.data(), size, &flags);
    }
};

// ============================================================================
// 基础属性
// ============================================================================

TEST_CASE("XdpRing init and size", "[hal][xdp_ring]") {
    LocalAddrRing r(16);
    CHECK(r.ring.size() == 16);
    CHECK(r.ring.empty());
}

TEST_CASE("XdpRing default constructed is empty", "[hal][xdp_ring]") {
    AddrRing ring;
    CHECK(ring.size() == 0);
}

// ============================================================================
// 生产者操作: reserve + submit
// ============================================================================

TEST_CASE("XdpRing reserve on empty ring", "[hal][xdp_ring]") {
    LocalAddrRing r(8);

    SECTION("Reserve up to capacity") {
        uint32_t n = r.ring.reserve(8);
        CHECK(n == 8);
    }

    SECTION("Reserve more than capacity") {
        uint32_t n = r.ring.reserve(16);
        CHECK(n == 8);
    }

    SECTION("Reserve zero") {
        uint32_t n = r.ring.reserve(0);
        CHECK(n == 0);
    }
}

TEST_CASE("XdpRing submit advances producer", "[hal][xdp_ring]") {
    LocalAddrRing r(8);

    uint32_t n = r.ring.reserve(4);
    REQUIRE(n == 4);

    for (uint32_t i = 0; i < 4; ++i) {
        r.ring.ring_at(i) = (i + 1) * 4096;
    }
    r.ring.submit(4);

    CHECK(r.producer == 4);
    CHECK_FALSE(r.ring.empty());
}

TEST_CASE("XdpRing ring_at wraps around", "[hal][xdp_ring]") {
    LocalAddrRing r(4); // mask = 3

    // 先推进 producer 到接近 wrap
    r.producer = 3;
    r.consumer = 3;

    uint32_t n = r.ring.reserve(4);
    REQUIRE(n == 4);

    // 写入 4 个值，应该 wrap 到 index 3,0,1,2
    for (uint32_t i = 0; i < 4; ++i) {
        r.ring.ring_at(i) = 100 + i;
    }
    r.ring.submit(4);

    CHECK(r.descs[3] == 100); // (3+0) & 3 = 3
    CHECK(r.descs[0] == 101); // (3+1) & 3 = 0
    CHECK(r.descs[1] == 102); // (3+2) & 3 = 1
    CHECK(r.descs[2] == 103); // (3+3) & 3 = 2
}

// ============================================================================
// 消费者操作: peek + release
// ============================================================================

TEST_CASE("XdpRing peek on empty ring", "[hal][xdp_ring]") {
    LocalAddrRing r(8);
    CHECK(r.ring.peek(8) == 0);
}

TEST_CASE("XdpRing peek after submit", "[hal][xdp_ring]") {
    LocalAddrRing r(8);

    // 生产 3 个
    uint32_t n = r.ring.reserve(3);
    for (uint32_t i = 0; i < n; ++i) {
        r.ring.ring_at(i) = i * 4096;
    }
    r.ring.submit(3);

    // 消费
    uint32_t avail = r.ring.peek(8);
    CHECK(avail == 3);

    for (uint32_t i = 0; i < avail; ++i) {
        CHECK(r.ring.ring_at_consumer(i) == i * 4096);
    }
}

TEST_CASE("XdpRing release advances consumer", "[hal][xdp_ring]") {
    LocalAddrRing r(8);

    // 生产 4 个
    r.ring.reserve(4);
    for (uint32_t i = 0; i < 4; ++i) r.ring.ring_at(i) = i;
    r.ring.submit(4);

    // 消费 2 个
    r.ring.peek(2);
    r.ring.release(2);

    CHECK(r.consumer == 2);

    // 还剩 2 个
    uint32_t avail = r.ring.peek(8);
    CHECK(avail == 2);
    CHECK(r.ring.ring_at_consumer(0) == 2);
    CHECK(r.ring.ring_at_consumer(1) == 3);
}

// ============================================================================
// 生产-消费循环
// ============================================================================

TEST_CASE("XdpRing producer-consumer cycle", "[hal][xdp_ring]") {
    LocalAddrRing r(4);

    // 填满
    uint32_t n = r.ring.reserve(4);
    CHECK(n == 4);
    for (uint32_t i = 0; i < 4; ++i) r.ring.ring_at(i) = i * 100;
    r.ring.submit(4);

    // 满了，不能再 reserve
    CHECK(r.ring.reserve(1) == 0);

    // 消费 2 个
    uint32_t avail = r.ring.peek(2);
    CHECK(avail == 2);
    CHECK(r.ring.ring_at_consumer(0) == 0);
    CHECK(r.ring.ring_at_consumer(1) == 100);
    r.ring.release(2);

    // 现在可以再 reserve 2 个
    n = r.ring.reserve(4);
    CHECK(n == 2);
    r.ring.ring_at(0) = 400;
    r.ring.ring_at(1) = 500;
    r.ring.submit(2);

    // 消费剩余 4 个
    avail = r.ring.peek(8);
    CHECK(avail == 4);
    CHECK(r.ring.ring_at_consumer(0) == 200);
    CHECK(r.ring.ring_at_consumer(1) == 300);
    CHECK(r.ring.ring_at_consumer(2) == 400);
    CHECK(r.ring.ring_at_consumer(3) == 500);
    r.ring.release(4);

    CHECK(r.ring.empty());
}

TEST_CASE("XdpRing many iterations with wrap", "[hal][xdp_ring]") {
    LocalAddrRing r(4);

    // 做 100 轮 produce-consume，验证 wrap 不出错
    for (uint64_t round = 0; round < 100; ++round) {
        uint32_t n = r.ring.reserve(2);
        REQUIRE(n == 2);
        r.ring.ring_at(0) = round * 2;
        r.ring.ring_at(1) = round * 2 + 1;
        r.ring.submit(2);

        uint32_t avail = r.ring.peek(2);
        REQUIRE(avail == 2);
        CHECK(r.ring.ring_at_consumer(0) == round * 2);
        CHECK(r.ring.ring_at_consumer(1) == round * 2 + 1);
        r.ring.release(2);
    }

    CHECK(r.ring.empty());
    CHECK(r.producer == 200);
    CHECK(r.consumer == 200);
}

// ============================================================================
// DescRing (模拟 TX/RX ring)
// ============================================================================

TEST_CASE("XdpRing with desc type", "[hal][xdp_ring]") {
    LocalDescRing r(8);

    uint32_t n = r.ring.reserve(2);
    REQUIRE(n == 2);

    r.ring.ring_at(0) = {0x0000, 64, 0};
    r.ring.ring_at(1) = {0x1000, 128, 0};
    r.ring.submit(2);

    uint32_t avail = r.ring.peek(8);
    CHECK(avail == 2);

    auto& d0 = r.ring.ring_at_consumer(0);
    auto& d1 = r.ring.ring_at_consumer(1);
    CHECK(d0.addr == 0x0000);
    CHECK(d0.len == 64);
    CHECK(d1.addr == 0x1000);
    CHECK(d1.len == 128);

    r.ring.release(2);
    CHECK(r.ring.empty());
}

// ============================================================================
// needs_wakeup 标志
// ============================================================================

TEST_CASE("XdpRing needs_wakeup", "[hal][xdp_ring]") {
    LocalAddrRing r(4);

    SECTION("Default flags = 0, no wakeup needed") {
        CHECK_FALSE(r.ring.needs_wakeup());
    }

    SECTION("XDP_RING_NEED_WAKEUP set") {
        r.flags = XDP_RING_NEED_WAKEUP;
        CHECK(r.ring.needs_wakeup());
    }

    SECTION("Other flags don't trigger wakeup") {
        r.flags = 0x02; // some other flag
        CHECK_FALSE(r.ring.needs_wakeup());
    }
}

// ============================================================================
// 无 flags 指针的 ring
// ============================================================================

TEST_CASE("XdpRing without flags pointer", "[hal][xdp_ring]") {
    uint32_t producer = 0, consumer = 0;
    uint64_t descs[4] = {};
    AddrRing ring;
    ring.init(&producer, &consumer, descs, 4, nullptr);

    CHECK_FALSE(ring.needs_wakeup());
    CHECK(ring.size() == 4);

    // 正常操作不受影响
    uint32_t n = ring.reserve(2);
    CHECK(n == 2);
    ring.ring_at(0) = 42;
    ring.submit(1);
    CHECK_FALSE(ring.empty());
}
