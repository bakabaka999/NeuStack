// tests/unit/hal/test_afxdp_config.cpp
// 测试 AFXDPConfig 默认值和 LinuxAFXDPDevice 配置逻辑
// 纯逻辑测试，不需要真实网卡或 root 权限

#include <catch2/catch_test_macros.hpp>

#ifdef NEUSTACK_PLATFORM_LINUX

#include "neustack/hal/hal_linux_afxdp.hpp"

using namespace neustack;

// ============================================================================
// AFXDPConfig 默认值测试
// ============================================================================

TEST_CASE("AFXDPConfig default values", "[hal][afxdp]") {
    AFXDPConfig cfg;

    SECTION("network interface defaults") {
        CHECK(cfg.ifname == "eth0");
        CHECK(cfg.queue_id == 0);
    }

    SECTION("L2 addresses default to zero") {
        for (int i = 0; i < 6; ++i) {
            CHECK(cfg.local_mac[i] == 0);
            CHECK(cfg.gateway_mac[i] == 0);
        }
    }

    SECTION("UMEM defaults") {
        CHECK(cfg.frame_count == 4096);
        CHECK(cfg.frame_size == 4096);
        CHECK(cfg.headroom == 0);
    }

    SECTION("ring size defaults are powers of two") {
        CHECK(cfg.fill_ring_size == 2048);
        CHECK(cfg.comp_ring_size == 2048);
        CHECK(cfg.rx_ring_size == 2048);
        CHECK(cfg.tx_ring_size == 2048);

        // 确保是 2 的幂
        CHECK((cfg.fill_ring_size & (cfg.fill_ring_size - 1)) == 0);
        CHECK((cfg.rx_ring_size & (cfg.rx_ring_size - 1)) == 0);
    }

    SECTION("behavior defaults") {
        CHECK(cfg.batch_size == 64);
        CHECK(cfg.zero_copy == true);
        CHECK(cfg.busy_poll == false);
        CHECK(cfg.busy_poll_budget == 64);
    }

    SECTION("BPF defaults") {
        CHECK(cfg.bpf_prog_path.empty());
        CHECK(cfg.force_native_mode == false);
    }
}

TEST_CASE("AFXDPConfig custom values", "[hal][afxdp]") {
    AFXDPConfig cfg;
    cfg.ifname = "enp3s0";
    cfg.queue_id = 2;
    cfg.frame_count = 8192;
    cfg.frame_size = 2048;
    cfg.batch_size = 128;
    cfg.zero_copy = false;
    cfg.busy_poll = true;
    cfg.busy_poll_budget = 32;
    cfg.bpf_prog_path = "/opt/xdp/custom.o";
    cfg.force_native_mode = true;

    CHECK(cfg.ifname == "enp3s0");
    CHECK(cfg.queue_id == 2);
    CHECK(cfg.frame_count == 8192);
    CHECK(cfg.frame_size == 2048);
    CHECK(cfg.batch_size == 128);
    CHECK(cfg.zero_copy == false);
    CHECK(cfg.busy_poll == true);
    CHECK(cfg.busy_poll_budget == 32);
    CHECK(cfg.bpf_prog_path == "/opt/xdp/custom.o");
    CHECK(cfg.force_native_mode == true);
}

TEST_CASE("AFXDPConfig MAC address assignment", "[hal][afxdp]") {
    AFXDPConfig cfg;

    // 模拟设置 MAC 地址
    cfg.local_mac = {0x02, 0x42, 0xAC, 0x11, 0x00, 0x02};
    cfg.gateway_mac = {0x02, 0x42, 0xAC, 0x11, 0x00, 0x01};

    CHECK(cfg.local_mac[0] == 0x02);
    CHECK(cfg.local_mac[5] == 0x02);
    CHECK(cfg.gateway_mac[5] == 0x01);
}

TEST_CASE("LinuxAFXDPDevice construction with config", "[hal][afxdp]") {
    AFXDPConfig cfg;
    cfg.ifname = "lo";

    LinuxAFXDPDevice dev(cfg);

    CHECK(dev.get_name() == "lo");
    CHECK(dev.get_fd() == -1);  // 尚未 open
    CHECK(dev.supports_zero_copy() == true);
    CHECK(dev.supports_batch() == true);
}

TEST_CASE("LinuxAFXDPDevice default construction", "[hal][afxdp]") {
    LinuxAFXDPDevice dev;

    CHECK(dev.get_name() == "eth0");
    CHECK(dev.get_fd() == -1);
}

TEST_CASE("LinuxAFXDPDevice stats initial values", "[hal][afxdp]") {
    LinuxAFXDPDevice dev;
    auto s = dev.stats();

    CHECK(s.rx_packets == 0);
    CHECK(s.tx_packets == 0);
    CHECK(s.rx_dropped == 0);
    CHECK(s.tx_dropped == 0);
    CHECK(s.fill_ring_empty == 0);
    CHECK(s.invalid_descs == 0);
}

TEST_CASE("LinuxAFXDPDevice close without open is safe", "[hal][afxdp]") {
    LinuxAFXDPDevice dev;
    // close() on an un-opened device should not crash
    CHECK(dev.close() == 0);
}

#endif // NEUSTACK_PLATFORM_LINUX
