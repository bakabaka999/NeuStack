// tests/unit/hal/test_batch_device.cpp
// 测试 device.hpp 中 v1.4 新增的批量收发接口：
//   PacketDesc, supports_batch(), supports_zero_copy(),
//   recv_batch(), send_batch(), release_rx(), poll()

#include <catch2/catch_test_macros.hpp>
#include "neustack/hal/device.hpp"
#include <vector>
#include <cstring>

using namespace neustack;

// ============================================================================
// Mock 设备：模拟批量收发（如 AF_XDP）
// ============================================================================

class MockBatchDevice : public NetDevice {
    std::vector<std::vector<uint8_t>> _pending_rx;
    std::vector<std::vector<uint8_t>> _consumed_rx;  // 保持 data 指针有效，模拟 UMEM
    std::vector<std::vector<uint8_t>> _sent_tx;
    std::vector<uint32_t> _released_addrs;
    int _poll_result = POLL_RX;

public:
    bool supports_batch() const override { return true; }
    bool supports_zero_copy() const override { return true; }

    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock_batch0"; }

    ssize_t send(const uint8_t* data, size_t len) override {
        _sent_tx.emplace_back(data, data + len);
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t* buf, size_t len, int /*timeout_ms*/) override {
        if (_pending_rx.empty()) return 0;
        auto& pkt = _pending_rx.front();
        size_t n = std::min(len, pkt.size());
        std::memcpy(buf, pkt.data(), n);
        _pending_rx.erase(_pending_rx.begin());
        return static_cast<ssize_t>(n);
    }

    uint32_t recv_batch(PacketDesc* descs, uint32_t max_count) override {
        uint32_t count = std::min(max_count, static_cast<uint32_t>(_pending_rx.size()));
        for (uint32_t i = 0; i < count; ++i) {
            // 先移到 _consumed_rx 保持内存有效（模拟 UMEM 零拷贝语义）
            _consumed_rx.push_back(std::move(_pending_rx[i]));
            auto& held = _consumed_rx.back();
            descs[i].data = held.data();
            descs[i].len = static_cast<uint32_t>(held.size());
            descs[i].addr = static_cast<uint32_t>(_consumed_rx.size() - 1) * 4096;
            descs[i].port_id = 0;
            descs[i].flags = PacketDesc::FLAG_ZEROCOPY;
        }
        _pending_rx.erase(_pending_rx.begin(), _pending_rx.begin() + count);
        return count;
    }

    uint32_t send_batch(const PacketDesc* descs, uint32_t count) override {
        for (uint32_t i = 0; i < count; ++i) {
            _sent_tx.emplace_back(descs[i].data, descs[i].data + descs[i].len);
        }
        return count;
    }

    void release_rx(const PacketDesc* descs, uint32_t count) override {
        for (uint32_t i = 0; i < count; ++i) {
            _released_addrs.push_back(descs[i].addr);
        }
    }

    int poll(int /*timeout_ms*/) override {
        return _poll_result;
    }

    // ─── 测试辅助方法 ───

    void inject_packet(const uint8_t* data, size_t len) {
        _pending_rx.emplace_back(data, data + len);
    }

    void inject_packet(const std::vector<uint8_t>& pkt) {
        _pending_rx.push_back(pkt);
    }

    const std::vector<std::vector<uint8_t>>& sent_packets() const { return _sent_tx; }
    const std::vector<uint32_t>& released_addrs() const { return _released_addrs; }
    void set_poll_result(int result) { _poll_result = result; }
};

// ============================================================================
// Mock 设备：模拟 TUN（不支持批量，走默认实现）
// ============================================================================

class MockTunDevice : public NetDevice {
    std::vector<std::vector<uint8_t>> _pending_rx;
    std::vector<std::vector<uint8_t>> _sent_tx;

public:
    // 不覆盖 supports_batch/supports_zero_copy → 默认返回 false

    int open() override { return 0; }
    int close() override { return 0; }
    int get_fd() const override { return -1; }
    std::string get_name() const override { return "mock_tun0"; }

    ssize_t send(const uint8_t* data, size_t len) override {
        _sent_tx.emplace_back(data, data + len);
        return static_cast<ssize_t>(len);
    }

    ssize_t recv(uint8_t* buf, size_t len, int /*timeout_ms*/) override {
        if (_pending_rx.empty()) return 0;
        auto& pkt = _pending_rx.front();
        size_t n = std::min(len, pkt.size());
        std::memcpy(buf, pkt.data(), n);
        _pending_rx.erase(_pending_rx.begin());
        return static_cast<ssize_t>(n);
    }

    void inject_packet(const std::vector<uint8_t>& pkt) {
        _pending_rx.push_back(pkt);
    }

    const std::vector<std::vector<uint8_t>>& sent_packets() const { return _sent_tx; }
};

// ============================================================================
// PacketDesc 结构体测试
// ============================================================================

TEST_CASE("PacketDesc flag operations", "[hal][batch]") {
    SECTION("Default flags are zero") {
        PacketDesc desc{};
        CHECK(desc.flags == 0);
        CHECK_FALSE(desc.is_zerocopy());
    }

    SECTION("FLAG_ZEROCOPY") {
        PacketDesc desc{};
        desc.flags = PacketDesc::FLAG_ZEROCOPY;
        CHECK(desc.is_zerocopy());
    }

    SECTION("FLAG_NEED_FREE") {
        PacketDesc desc{};
        desc.flags = PacketDesc::FLAG_NEED_FREE;
        CHECK_FALSE(desc.is_zerocopy());
        CHECK((desc.flags & PacketDesc::FLAG_NEED_FREE) != 0);
    }

    SECTION("Combined flags") {
        PacketDesc desc{};
        desc.flags = PacketDesc::FLAG_ZEROCOPY | PacketDesc::FLAG_NEED_FREE;
        CHECK(desc.is_zerocopy());
        CHECK((desc.flags & PacketDesc::FLAG_NEED_FREE) != 0);
    }
}

// ============================================================================
// 能力查询测试
// ============================================================================

TEST_CASE("Device capability queries", "[hal][batch]") {
    SECTION("MockBatchDevice reports batch + zero-copy") {
        MockBatchDevice dev;
        CHECK(dev.supports_batch());
        CHECK(dev.supports_zero_copy());
    }

    SECTION("MockTunDevice reports neither") {
        MockTunDevice dev;
        CHECK_FALSE(dev.supports_batch());
        CHECK_FALSE(dev.supports_zero_copy());
    }
}

// ============================================================================
// 批量接收测试 (MockBatchDevice)
// ============================================================================

TEST_CASE("Batch recv with MockBatchDevice", "[hal][batch]") {
    MockBatchDevice dev;

    SECTION("Empty device returns 0") {
        PacketDesc descs[8];
        CHECK(dev.recv_batch(descs, 8) == 0);
    }

    SECTION("Single packet") {
        uint8_t pkt[] = {0x45, 0x00, 0x00, 0x28};
        dev.inject_packet(pkt, sizeof(pkt));

        PacketDesc descs[8];
        uint32_t count = dev.recv_batch(descs, 8);
        CHECK(count == 1);
        CHECK(descs[0].len == sizeof(pkt));
        CHECK(std::memcmp(descs[0].data, pkt, sizeof(pkt)) == 0);
        CHECK(descs[0].is_zerocopy());
    }

    SECTION("Multiple packets") {
        std::vector<uint8_t> pkt1 = {1, 2, 3, 4};
        std::vector<uint8_t> pkt2 = {5, 6, 7, 8, 9, 10};
        std::vector<uint8_t> pkt3 = {11};
        dev.inject_packet(pkt1);
        dev.inject_packet(pkt2);
        dev.inject_packet(pkt3);

        PacketDesc descs[8];
        uint32_t count = dev.recv_batch(descs, 8);
        CHECK(count == 3);
        CHECK(descs[0].len == 4);
        CHECK(descs[1].len == 6);
        CHECK(descs[2].len == 1);
    }

    SECTION("max_count limits returned packets") {
        for (int i = 0; i < 10; ++i) {
            std::vector<uint8_t> pkt = {static_cast<uint8_t>(i)};
            dev.inject_packet(pkt);
        }

        PacketDesc descs[4];
        uint32_t count = dev.recv_batch(descs, 4);
        CHECK(count == 4);

        // 剩余 6 个包仍在队列中
        count = dev.recv_batch(descs, 4);
        CHECK(count == 4);

        count = dev.recv_batch(descs, 4);
        CHECK(count == 2);

        count = dev.recv_batch(descs, 4);
        CHECK(count == 0);
    }

    SECTION("max_count=0 returns 0") {
        dev.inject_packet({1, 2, 3});
        PacketDesc descs[1];
        CHECK(dev.recv_batch(descs, 0) == 0);
    }
}

// ============================================================================
// 批量发送测试 (MockBatchDevice)
// ============================================================================

TEST_CASE("Batch send with MockBatchDevice", "[hal][batch]") {
    MockBatchDevice dev;

    SECTION("Send single packet") {
        uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
        PacketDesc desc{};
        desc.data = data;
        desc.len = sizeof(data);

        uint32_t sent = dev.send_batch(&desc, 1);
        CHECK(sent == 1);
        CHECK(dev.sent_packets().size() == 1);
        CHECK(dev.sent_packets()[0].size() == 4);
        CHECK(dev.sent_packets()[0][0] == 0xDE);
    }

    SECTION("Send multiple packets") {
        uint8_t data1[] = {1, 2};
        uint8_t data2[] = {3, 4, 5};
        PacketDesc descs[2];
        descs[0] = {data1, 2, 0, 0, 0};
        descs[1] = {data2, 3, 0, 0, 0};

        uint32_t sent = dev.send_batch(descs, 2);
        CHECK(sent == 2);
        CHECK(dev.sent_packets().size() == 2);
        CHECK(dev.sent_packets()[0].size() == 2);
        CHECK(dev.sent_packets()[1].size() == 3);
    }

    SECTION("Send zero packets") {
        CHECK(dev.send_batch(nullptr, 0) == 0);
    }
}

// ============================================================================
// release_rx 测试
// ============================================================================

TEST_CASE("release_rx tracks freed frames", "[hal][batch]") {
    MockBatchDevice dev;

    dev.inject_packet({1, 2, 3});
    dev.inject_packet({4, 5, 6});

    PacketDesc descs[4];
    uint32_t count = dev.recv_batch(descs, 4);
    REQUIRE(count == 2);

    dev.release_rx(descs, count);

    CHECK(dev.released_addrs().size() == 2);
    CHECK(dev.released_addrs()[0] == 0 * 4096);
    CHECK(dev.released_addrs()[1] == 1 * 4096);
}

// ============================================================================
// poll 测试
// ============================================================================

TEST_CASE("poll returns configured events", "[hal][batch]") {
    MockBatchDevice dev;

    SECTION("Default returns POLL_RX") {
        CHECK(dev.poll(10) == NetDevice::POLL_RX);
    }

    SECTION("Can set poll result") {
        dev.set_poll_result(NetDevice::POLL_RX | NetDevice::POLL_TX);
        CHECK((dev.poll(0) & NetDevice::POLL_RX) != 0);
        CHECK((dev.poll(0) & NetDevice::POLL_TX) != 0);
    }

    SECTION("No events") {
        dev.set_poll_result(0);
        CHECK(dev.poll(10) == 0);
    }
}

// ============================================================================
// 默认实现回退测试 (MockTunDevice — 不覆盖 batch 方法)
// ============================================================================

TEST_CASE("Default recv_batch falls back to recv", "[hal][batch]") {
    MockTunDevice dev;

    SECTION("Returns 0 when no data") {
        PacketDesc descs[4];
        CHECK(dev.recv_batch(descs, 4) == 0);
    }

    SECTION("Returns 1 packet via default implementation") {
        dev.inject_packet({0x45, 0x00, 0x00, 0x14});

        PacketDesc descs[4];
        uint32_t count = dev.recv_batch(descs, 4);
        CHECK(count == 1);
        CHECK(descs[0].len == 4);
        CHECK_FALSE(descs[0].is_zerocopy());
        CHECK(descs[0].addr == 0);
        CHECK(descs[0].port_id == 0);
    }

    SECTION("Only one packet per call even if multiple pending") {
        dev.inject_packet({1, 2});
        dev.inject_packet({3, 4});

        PacketDesc descs[4];
        // 默认实现每次只调一次 recv()，所以只返回 1 个包
        uint32_t count = dev.recv_batch(descs, 4);
        CHECK(count == 1);

        // 第二次调用拿到第二个包
        count = dev.recv_batch(descs, 4);
        CHECK(count == 1);

        // 没了
        count = dev.recv_batch(descs, 4);
        CHECK(count == 0);
    }

    SECTION("max_count=0 returns 0") {
        dev.inject_packet({1});
        PacketDesc descs[1];
        CHECK(dev.recv_batch(descs, 0) == 0);
    }
}

TEST_CASE("Default send_batch falls back to send", "[hal][batch]") {
    MockTunDevice dev;

    SECTION("Sends via per-packet send()") {
        uint8_t data1[] = {0xAA, 0xBB};
        uint8_t data2[] = {0xCC};
        PacketDesc descs[2];
        descs[0] = {data1, 2, 0, 0, 0};
        descs[1] = {data2, 1, 0, 0, 0};

        uint32_t sent = dev.send_batch(descs, 2);
        CHECK(sent == 2);
        CHECK(dev.sent_packets().size() == 2);
        CHECK(dev.sent_packets()[0] == std::vector<uint8_t>{0xAA, 0xBB});
        CHECK(dev.sent_packets()[1] == std::vector<uint8_t>{0xCC});
    }
}

TEST_CASE("Default release_rx is no-op", "[hal][batch]") {
    MockTunDevice dev;
    // MockTunDevice 不覆盖 release_rx，使用基类默认空实现
    PacketDesc desc{};
    dev.release_rx(&desc, 1);  // 不应崩溃
}

TEST_CASE("Default poll returns POLL_RX", "[hal][batch]") {
    MockTunDevice dev;
    // MockTunDevice 不覆盖 poll，使用基类默认实现
    CHECK(dev.poll(100) == NetDevice::POLL_RX);
}

// ============================================================================
// 事件循环模拟 — 验证批量路径和兼容路径产生相同结果
// ============================================================================

TEST_CASE("Batch and non-batch paths produce same results", "[hal][batch]") {
    // 准备测试数据
    std::vector<std::vector<uint8_t>> test_packets = {
        {0x45, 0x00, 0x00, 0x14, 0x00, 0x01},
        {0x45, 0x00, 0x00, 0x20, 0x00, 0x02, 0xAA, 0xBB},
        {0x45, 0x00, 0x00, 0x0C},
    };

    // 用 batch 设备收包
    MockBatchDevice batch_dev;
    for (auto& pkt : test_packets) batch_dev.inject_packet(pkt);

    PacketDesc batch_descs[8];
    uint32_t batch_count = batch_dev.recv_batch(batch_descs, 8);

    // 用 TUN 设备逐包收
    MockTunDevice tun_dev;
    for (auto& pkt : test_packets) tun_dev.inject_packet(pkt);

    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tun_results;
    PacketDesc tun_descs[1];
    while (true) {
        uint32_t count = tun_dev.recv_batch(tun_descs, 1);
        if (count == 0) break;
        tun_results.emplace_back(
            tun_descs[0].len,
            std::vector<uint8_t>(tun_descs[0].data, tun_descs[0].data + tun_descs[0].len)
        );
    }

    // 验证结果一致
    REQUIRE(batch_count == test_packets.size());
    REQUIRE(tun_results.size() == test_packets.size());

    for (size_t i = 0; i < test_packets.size(); ++i) {
        CHECK(batch_descs[i].len == test_packets[i].size());
        CHECK(tun_results[i].first == test_packets[i].size());
        CHECK(std::memcmp(batch_descs[i].data, test_packets[i].data(),
                          test_packets[i].size()) == 0);
        CHECK(tun_results[i].second == test_packets[i]);
    }
}
