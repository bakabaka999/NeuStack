#include <catch2/catch_test_macros.hpp>
#include "neustack/metrics/global_metrics.hpp"
#include <cstdint>

using namespace neustack;

TEST_CASE("GlobalMetrics: Atomic counter behavior and snapshots", "[metrics][global_metrics]") {
    // 为了保证测试隔离性，不使用全局单例 global_metrics()，而是构造本地实例
    GlobalMetrics metrics;

    SECTION("Initial state is zero") {
        auto snap = metrics.snapshot();
        
        CHECK(snap.packets_rx == 0);
        CHECK(snap.packets_tx == 0);
        CHECK(snap.bytes_rx == 0);
        CHECK(snap.bytes_tx == 0);
        CHECK(snap.syn_received == 0);
        CHECK(snap.rst_received == 0);
        CHECK(snap.fin_received == 0);
        CHECK(snap.active_connections == 0);
        CHECK(snap.conn_established == 0);
        CHECK(snap.conn_closed == 0);
        CHECK(snap.conn_reset == 0);
        CHECK(snap.conn_timeout == 0);
        CHECK(snap.total_retransmits == 0);
    }

    SECTION("Snapshot reflects cumulative increments") {
        // 模拟网络活动
        metrics.packets_rx += 10;
        metrics.packets_tx += 5;
        metrics.bytes_rx += 1024;
        metrics.bytes_tx += 512;
        
        metrics.syn_received += 2;
        metrics.rst_received += 1;
        metrics.fin_received += 1;
        
        metrics.conn_established += 1;
        metrics.conn_timeout += 1;
        metrics.total_retransmits += 3;

        auto snap = metrics.snapshot();
        
        CHECK(snap.packets_rx == 10);
        CHECK(snap.packets_tx == 5);
        CHECK(snap.bytes_rx == 1024);
        CHECK(snap.bytes_tx == 512);
        CHECK(snap.syn_received == 2);
        CHECK(snap.rst_received == 1);
        CHECK(snap.conn_established == 1);
        CHECK(snap.conn_timeout == 1);
        CHECK(snap.total_retransmits == 3);
    }

    SECTION("Active connections counter (uint32) support increments and decrements") {
        metrics.active_connections++;
        metrics.active_connections++;
        CHECK(metrics.snapshot().active_connections == 2);

        metrics.active_connections--;
        CHECK(metrics.snapshot().active_connections == 1);
        
        metrics.active_connections--;
        CHECK(metrics.snapshot().active_connections == 0);
    }
}

TEST_CASE("GlobalMetrics: Snapshot Delta (diff) calculation", "[metrics][global_metrics]") {
    GlobalMetrics metrics;

    SECTION("Delta correctly calculates difference between two snapshots") {
        // 获取初始基线快照
        metrics.packets_rx += 100; // 预设一些初始值
        auto snap_v1 = metrics.snapshot();

        // 发生第二波网络活动
        metrics.packets_rx += 50;
        metrics.packets_tx += 20;
        metrics.bytes_rx += 5000;
        metrics.bytes_tx += 2000;
        metrics.syn_received += 10;
        metrics.rst_received += 2;
        metrics.conn_established += 5;
        metrics.conn_reset += 1;

        // 获取当前快照
        auto snap_v2 = metrics.snapshot();

        // 计算差值 (Delta)
        auto delta = snap_v2.diff(snap_v1);

        // 验证 Delta 字段
        CHECK(delta.packets_rx == 50);
        CHECK(delta.packets_tx == 20);
        CHECK(delta.bytes_rx == 5000);
        CHECK(delta.bytes_tx == 2000);
        CHECK(delta.syn_received == 10);
        CHECK(delta.rst_received == 2);
        CHECK(delta.conn_established == 5);
        CHECK(delta.conn_reset == 1);
    }

    SECTION("Delta with no activity should be zero") {
        auto snap1 = metrics.snapshot();
        auto snap2 = metrics.snapshot();
        auto delta = snap2.diff(snap1);

        CHECK(delta.packets_rx == 0);
        CHECK(delta.syn_received == 0);
        CHECK(delta.conn_established == 0);
    }
}
