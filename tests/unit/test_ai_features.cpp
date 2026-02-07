#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "neustack/metrics/ai_features.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/tcp_sample.hpp"
#include <vector>

using namespace neustack;
using Catch::Matchers::WithinAbs;

// Helper: construct a TCPSample with common fields
static TCPSample make_sample(uint32_t rtt_us, uint32_t min_rtt_us,
                              uint32_t delivery_rate, uint32_t cwnd,
                              uint32_t bytes_in_flight,
                              uint16_t packets_sent, uint16_t packets_lost) {
    TCPSample s{};
    s.rtt_us = rtt_us;
    s.min_rtt_us = min_rtt_us;
    s.delivery_rate = delivery_rate;
    s.cwnd = cwnd;
    s.bytes_in_flight = bytes_in_flight;
    s.packets_sent = packets_sent;
    s.packets_lost = packets_lost;
    return s;
}

TEST_CASE("OrcaFeatures: Congestion control features", "[metrics][ai]") {
    auto s = make_sample(11000, 10000, 5000000, 100, 50000, 1000, 10);
    uint32_t est_bw = 6000000;
    float predicted_bw = 5500000.0f;

    SECTION("Normal feature calculation") {
        auto features = OrcaFeatures::from_sample(s, est_bw, predicted_bw);
        auto vec = features.to_vector();

        REQUIRE(vec.size() == OrcaFeatures::dim());
        REQUIRE(OrcaFeatures::dim() == 7);

        // throughput: 5e6 / 6e6 = 0.8333
        CHECK_THAT(vec[0], WithinAbs(0.8333, 0.001));
        // queuing_delay: (11000-10000)/10000 = 0.1
        CHECK_THAT(vec[1], WithinAbs(0.1, 0.001));
        // rtt_ratio: 11000/10000 = 1.1
        CHECK_THAT(vec[2], WithinAbs(1.1, 0.001));
        // loss_rate: 10/1000 = 0.01
        CHECK_THAT(vec[3], WithinAbs(0.01, 0.001));
        // cwnd_norm: BDP = 6e6*10000/1e6/1460 ≈ 41.096, 100/41.096 ≈ 2.433
        CHECK_THAT(vec[4], WithinAbs(2.433, 0.01));
        // in_flight_ratio: 50000/(100*1460) = 0.3424
        CHECK_THAT(vec[5], WithinAbs(0.3424, 0.001));
        // predicted_bw: 5.5e6/10e6 = 0.55
        CHECK_THAT(vec[6], WithinAbs(0.55, 0.001));
    }

    SECTION("Clipping upper bounds") {
        s.delivery_rate = 20000000; // 20e6/6e6 > 2.0
        s.rtt_us = 100000;          // (100k-10k)/10k = 9.0 > 5.0
        auto vec = OrcaFeatures::from_sample(s, est_bw, predicted_bw).to_vector();

        CHECK(vec[0] == 2.0f);  // throughput clipped
        CHECK(vec[1] == 5.0f);  // queuing_delay clipped
        CHECK(vec[2] == 5.0f);  // rtt_ratio clipped
    }

    SECTION("Clipping lower bounds") {
        s.rtt_us = s.min_rtt_us; // queuing_delay = 0, rtt_ratio = 1
        auto vec = OrcaFeatures::from_sample(s, est_bw, predicted_bw).to_vector();

        CHECK(vec[1] == 0.0f);  // queuing_delay = 0
        CHECK(vec[2] == 1.0f);  // rtt_ratio = 1
    }

    SECTION("Zero est_bw / min_rtt / packets_sent / cwnd") {
        // est_bw=0
        auto vec1 = OrcaFeatures::from_sample(s, 0, 0.0f).to_vector();
        CHECK(vec1[0] == 0.0f);  // throughput → 0
        CHECK(vec1[4] == 1.0f);  // cwnd_norm with bdp=0 → 1.0 (fallback)

        // min_rtt=0
        s.min_rtt_us = 0;
        CHECK_NOTHROW(OrcaFeatures::from_sample(s, est_bw, predicted_bw));

        // packets_sent=0
        s.min_rtt_us = 10000;
        s.packets_sent = 0;
        auto vec2 = OrcaFeatures::from_sample(s, est_bw, predicted_bw).to_vector();
        CHECK(vec2[3] == 0.0f);  // loss_rate → 0

        // cwnd=0
        s.packets_sent = 1000;
        s.cwnd = 0;
        auto vec3 = OrcaFeatures::from_sample(s, est_bw, predicted_bw).to_vector();
        CHECK(vec3[5] == 0.0f);  // in_flight_ratio → 0
    }
}

TEST_CASE("AnomalyFeatures: Network anomaly detection", "[metrics][ai]") {
    using Delta = GlobalMetrics::Snapshot::Delta;
    Delta d{};
    d.packets_rx = 1000;
    d.packets_tx = 800;
    d.bytes_tx = 1000000;
    d.syn_received = 5;
    d.rst_received = 2;
    d.conn_established = 3;
    uint32_t active = 10;

    SECTION("Normal metrics all in [0,1]") {
        auto features = AnomalyFeatures::from_delta(d, active);
        auto vec = features.to_vector();
        REQUIRE(vec.size() == AnomalyFeatures::dim());
        REQUIRE(AnomalyFeatures::dim() == 8);

        for (float val : vec) {
            CHECK(val >= 0.0f);
            CHECK(val <= 1.0f);
        }
    }

    SECTION("Extreme values clipped to 1.0") {
        d.syn_received = 500; // 500/100 = 5.0 → clipped to 1.0
        auto features = AnomalyFeatures::from_delta(d, active);
        CHECK(features.syn_rate_norm == 1.0f);
    }

    SECTION("packets_rx=0 division safety") {
        d.packets_rx = 0;
        CHECK_NOTHROW(AnomalyFeatures::from_delta(d, active));
        auto features = AnomalyFeatures::from_delta(d, active);
        CHECK(features.tx_rx_ratio_norm == 0.0f);
    }
}

TEST_CASE("BandwidthFeatures: Time-series samples", "[metrics][ai]") {
    uint32_t min_rtt = 10000;

    SECTION("Single sample produces 3-element vector") {
        std::vector<TCPSample> samples;
        samples.push_back(make_sample(15000, 10000, 5000000, 50, 10000, 100, 0));

        auto features = BandwidthFeatures::from_samples(samples, min_rtt);
        auto vec = features.to_vector();

        // 1 sample → [throughput_0, rtt_0, loss_0] = 3 elements
        REQUIRE(vec.size() == 3);
        CHECK_THAT(vec[0], WithinAbs(0.5, 0.001));   // 5e6/10e6
        CHECK_THAT(vec[1], WithinAbs(1.5, 0.001));   // 15000/10000
        CHECK_THAT(vec[2], WithinAbs(0.0, 0.001));   // 0/100
    }

    SECTION("30 samples produce 90-element vector") {
        std::vector<TCPSample> samples;
        for (int i = 0; i < 30; ++i) {
            samples.push_back(make_sample(20000, 10000, 2000000, 50, 10000, 100, 0));
        }
        auto vec = BandwidthFeatures::from_samples(samples, min_rtt).to_vector();

        REQUIRE(vec.size() == 90);
        // Layout: [throughput_0..29, rtt_0..29, loss_0..29]
        CHECK_THAT(vec[0], WithinAbs(0.2, 0.001));    // throughput: 2e6/10e6
        CHECK_THAT(vec[30], WithinAbs(2.0, 0.001));   // rtt: 20000/10000
        CHECK_THAT(vec[60], WithinAbs(0.0, 0.001));   // loss: 0
    }

    SECTION("More than 30 samples — all kept") {
        std::vector<TCPSample> samples;
        for (int i = 0; i < 35; ++i) {
            samples.push_back(make_sample(20000, 10000, 2000000, 50, 10000, 100, 0));
        }
        auto vec = BandwidthFeatures::from_samples(samples, min_rtt).to_vector();
        // from_samples does not truncate; returns all 35
        REQUIRE(vec.size() == 105); // 35*3
    }
}
