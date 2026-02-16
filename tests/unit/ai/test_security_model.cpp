#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "neustack/ai/ai_model.hpp"
#include "neustack/metrics/security_exporter.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/metrics/security_features.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>

using namespace neustack;
using Catch::Matchers::WithinAbs;

// ============================================================================
// ISecurityModel::Input 测试
// ============================================================================

TEST_CASE("ISecurityModel::Input to_array", "[ai][security]") {
    ISecurityModel::Input input{
        .pps_norm           = 0.1f,
        .bps_norm           = 0.2f,
        .syn_rate_norm      = 0.3f,
        .rst_rate_norm      = 0.4f,
        .syn_ratio_norm     = 0.5f,
        .new_conn_rate_norm = 0.6f,
        .avg_pkt_size_norm  = 0.7f,
        .rst_ratio_norm     = 0.8f,
    };

    auto arr = input.to_array();
    REQUIRE(arr.size() == ISecurityModel::INPUT_DIM);
    CHECK_THAT(arr[0], WithinAbs(0.1, 1e-5));
    CHECK_THAT(arr[7], WithinAbs(0.8, 1e-5));
}

// ============================================================================
// SecurityFeatureExtractor → ISecurityModel::Input 转换测试
// ============================================================================

TEST_CASE("SecurityFeatures to SecurityModel input mapping", "[ai][security]") {
    SecurityFeatureExtractor extractor;
    SecurityMetrics metrics;

    // 模拟一些流量
    constexpr uint8_t TCP_SYN = 0x02;
    constexpr uint8_t TCP_RST = 0x04;
    for (int i = 0; i < 100; ++i) {
        metrics.record_packet(500, 0);    // 普通包
    }
    for (int i = 0; i < 20; ++i) {
        metrics.record_packet(60, TCP_SYN);
    }
    for (int i = 0; i < 5; ++i) {
        metrics.record_packet(40, TCP_RST);
    }

    // tick 让滑动窗口生效
    metrics.tick();

    auto snap = metrics.snapshot();
    auto features = extractor.extract(snap, 50);

    // 验证特征值在 [0, 1] 范围内
    CHECK(features.packets_rx_norm >= 0.0f);
    CHECK(features.packets_rx_norm <= 1.0f);
    CHECK(features.syn_rate_norm >= 0.0f);
    CHECK(features.syn_rate_norm <= 1.0f);
    CHECK(features.rst_rate_norm >= 0.0f);
    CHECK(features.rst_rate_norm <= 1.0f);
}

// ============================================================================
// SecurityFeatureExtractor::extract_security() 测试
// ============================================================================

TEST_CASE("extract_security: direct ISecurityModel::Input output", "[ai][security]") {
    SecurityFeatureExtractor extractor;
    SecurityMetrics metrics;

    constexpr uint8_t TCP_SYN = 0x02;
    constexpr uint8_t TCP_SYN_ACK = 0x12;
    constexpr uint8_t TCP_RST = 0x04;

    // 模拟混合流量
    for (int i = 0; i < 200; ++i) {
        metrics.record_packet(800, 0);     // 普通包
    }
    for (int i = 0; i < 50; ++i) {
        metrics.record_packet(60, TCP_SYN);
    }
    for (int i = 0; i < 10; ++i) {
        metrics.record_packet(60, TCP_SYN_ACK);
    }
    for (int i = 0; i < 15; ++i) {
        metrics.record_packet(40, TCP_RST);
    }
    metrics.tick();

    auto snap = metrics.snapshot();
    auto input = extractor.extract_security(snap);

    // 所有字段都在 [0, 1] 范围内
    auto arr = input.to_array();
    for (size_t i = 0; i < arr.size(); ++i) {
        CHECK(arr[i] >= 0.0f);
        CHECK(arr[i] <= 1.0f);
    }

    // pps > 0（有流量）
    CHECK(input.pps_norm > 0.0f);
    // syn_rate > 0（有 SYN 包）
    CHECK(input.syn_rate_norm > 0.0f);
    // rst_rate > 0（有 RST 包）
    CHECK(input.rst_rate_norm > 0.0f);
    // syn_ratio > 0.5（SYN/SYN-ACK = 50/10 = 5.0，超过 warning 阈值 5.0，sigmoid ≈ 0.5）
    CHECK(input.syn_ratio_norm >= 0.45f);
    // avg_pkt_size > 0
    CHECK(input.avg_pkt_size_norm > 0.0f);
    // rst_ratio > 0
    CHECK(input.rst_ratio_norm > 0.0f);
}

// ============================================================================
// SecurityExporter 测试
// ============================================================================

TEST_CASE("SecurityExporter: CSV output format", "[metrics][security]") {
    const std::string test_file = (std::filesystem::temp_directory_path() / "test_security_export.csv").string();
    // 清理
    std::remove(test_file.c_str());

    SecurityMetrics metrics;

    {
        SecurityExporter exporter(test_file, metrics);
        REQUIRE(exporter.is_open());

        // 模拟流量
        constexpr uint8_t TCP_SYN = 0x02;
        for (int i = 0; i < 50; ++i) {
            metrics.record_packet(200, 0);
        }
        for (int i = 0; i < 10; ++i) {
            metrics.record_packet(60, TCP_SYN);
        }
        metrics.tick();

        // 写入 2 行
        exporter.flush(0);  // 正常
        exporter.flush(1);  // 异常标注
        exporter.sync();

        CHECK(exporter.rows_written() == 2);
    }

    // 验证文件内容
    std::ifstream f(test_file);
    REQUIRE(f.is_open());

    std::string header;
    std::getline(f, header);
    CHECK(header.find("timestamp_ms") != std::string::npos);
    CHECK(header.find("label") != std::string::npos);

    // 第一行数据，label=0
    std::string line1;
    std::getline(f, line1);
    REQUIRE(!line1.empty());
    // 最后一个字段是 label
    CHECK(line1.back() == '0');

    // 第二行数据，label=1
    std::string line2;
    std::getline(f, line2);
    REQUIRE(!line2.empty());
    CHECK(line2.back() == '1');

    // 清理
    std::remove(test_file.c_str());
}

TEST_CASE("SecurityExporter: column count matches header", "[metrics][security]") {
    const std::string test_file = (std::filesystem::temp_directory_path() / "test_security_export_cols.csv").string();
    std::remove(test_file.c_str());

    SecurityMetrics metrics;

    {
        SecurityExporter exporter(test_file, metrics);
        metrics.record_packet(100, 0);
        metrics.tick();
        exporter.flush(0);
        exporter.sync();
    }

    std::ifstream f(test_file);
    std::string header, data;
    std::getline(f, header);
    std::getline(f, data);

    // 统计逗号数（列数 = 逗号数 + 1）
    auto count_commas = [](const std::string& s) {
        return static_cast<int>(std::count(s.begin(), s.end(), ','));
    };

    CHECK(count_commas(header) == count_commas(data));
    CHECK(count_commas(header) == 14);  // 15 列 → 14 逗号

    std::remove(test_file.c_str());
}
