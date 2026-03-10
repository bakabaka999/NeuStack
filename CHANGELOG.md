# Changelog

All notable changes to NeuStack will be documented in this file.

## [1.3.0] - 2025-03-10

### Added

**Telemetry Framework**
- `MetricsRegistry` singleton with zero-allocation `Counter`, `Gauge`, `Histogram` metric types (#17)
- Bridges existing `GlobalMetrics` and `SecurityMetrics` without modifying original interfaces
- `JsonExporter` and `PrometheusExporter` for pluggable metric serialization (#18)
- `TelemetryAPI` real-time query interface: traffic stats, TCP connections, RTT distribution, AI model status (#18)
- Shared `JsonBuilder` utility for zero-dependency JSON serialization

**HTTP Metrics Endpoints**
- Auto-registered endpoints on stack startup: `/api/v1/health`, `/api/v1/stats`, `/api/v1/stats/traffic`, `/api/v1/stats/tcp`, `/api/v1/stats/security`, `/api/v1/connections`, `/metrics` (Prometheus) (#19)
- Query parameter support (`?pretty=true`) with `HttpRequest` parser extension

**CLI Tools**
- `neustack-stat` standalone remote monitoring tool with live terminal dashboard, ANSI colors, cross-platform (Windows/Linux/macOS) (#19)
- `neustack_demo` full interactive rewrite: colored ASCII banner, prompt system, commands `ping`, `dns`, `get`, `post`, `nc`, `stats`, `conns`, `json`, `fw`, `h`, `q` (#19)

**Testing & Benchmarks**
- Unit tests for metrics registry, JSON exporter, Prometheus exporter, TelemetryAPI, HTTP endpoints
- `bench_metrics_hotpath` benchmark validating zero-allocation hot path

**Examples**
- Rewrote all 9 examples with uniform English style, consistent error handling, and meaningful demo content

### Fixed

**AI Anomaly Model**
- Redesigned 8 raw-counter features → volume-invariant ratio features (`log_pkt_rate`, `bytes_per_pkt`, `syn_ratio`, `rst_ratio`, `conn_completion`, `tx_rx_ratio`, `log_active_conn`, `log_conn_reset`), eliminating false positives on downloads and high-throughput transfers
- Added `_augment_normal_data()` to cover high-pps + low-syn patterns absent from collected training data
- Switched threshold from F1-optimal (skewed by mislabeled inter-attack idle samples) to manual calibration (0.01)

**Security AI Model**
- Replaced `sigmoid(syn_to_synack_ratio)` with `syn_rate / pps` (SYN fraction) — the old metric was unreliable in user-space stack where SYN-ACK packets are sent outbound but never counted as inbound, causing permanent false positives
- Updated normalization: `max_pps` 1000→20000, `max_bps` 500000→20000000 to match real deployment traffic
- Added `_augment_security_normal()` data augmentation for high-pps normal traffic patterns
- Retrained with p99 threshold calibration (0.005), maintaining 93%+ attack recall

### Changed
- Project version 1.2.0 → 1.3.0
- Anomaly features log level reverted from WARN to DEBUG (real alerts remain WARN)

---

## [1.2.0] - 2025-02-15

### Added

**AI Firewall Engine**
- Zero-allocation packet inspection engine with `FixedPool` pooled `PacketEvent`/`FirewallDecision` (#8)
- Rule engine: blacklist → whitelist → rate limiting → custom rules, priority-sorted (#9)
- Per-IP token bucket rate limiter with automatic expiry cleanup
- AI anomaly detection via ONNX inference with Shadow Mode (#10)
- Shadow Mode auto-escalation: consecutive anomalies → blocking, consecutive normals → shadow recovery
- Escalation cooldown to prevent oscillation under transient traffic spikes
- Passive monitoring: collect AI metrics even when firewall is disabled

**Security AI Model**
- `SecurityAnomalyModel` with 8-dimensional security features (pps, bps, syn_rate, rst_rate, syn_ratio, new_conn_rate, avg_pkt_size, rst_ratio) (#11)
- Deep Autoencoder architecture (8→64→32→4→32→64→8) with BatchNorm + GELU
- Full training pipeline: data cleaning, dataset preparation, training with cosine annealing + early stopping, ONNX export with threshold metadata (#12, #13)
- Pre-trained `security_anomaly.onnx` model included

**Data Collection**
- `SecurityExporter` for security metrics CSV export
- Automated collection scripts for macOS and Linux (normal + attack scenarios)
- Traffic generation scripts for security data collection
- Data merge and npz generation pipeline

**Testing**
- End-to-end firewall test suite: 16 test cases / 91 assertions covering rule priority, rate limiting, AI shadow/enforce mode, malformed packet handling, 10k packet stress test, dynamic rules, mixed protocols, bulk IP lists
- Firewall AI Shadow Mode integration test
- Firewall packet filtering integration test

**Demo & API**
- `GET /api/firewall/status` HTTP endpoint with full AI statistics
- `GET /api/info` dynamically lists enabled services (firewall, firewall-ai, ai-intelligence)
- Demo interactive commands: `f` (firewall stats), `fw shadow on/off`, `fw threshold <val>`, `fw bl add/del <ip>`
- Escalation/de-escalation counters in both CLI and API output

**Infrastructure**
- Docker playground for single-container TUN networking (#7)
- CI: AI build matrix (ON/OFF) with ONNX Runtime installation

### Fixed
- RateLimiter rehash crash: `unordered_map` iterator invalidation during erase
- `record_packet` missed on certain code paths, causing incomplete metrics
- `on_timer` changed to timestamp-driven to avoid timer frequency dependency
- Duplicate `--security-label` in demo CLI
- Duplicate `/api/firewall/status` route registration in demo
- Firewall inspect decision not displayed in event loop log

### Changed
- Project version 1.1.0 → 1.2.0
- Project description updated to include "Intelligent Firewall"
- CI now triggers on `dev` branch push

---

## [1.1.0] - 2025-02-08

### Added
- `FixedPool<T>` for zero-allocation object management (#2)
- TCP connection, HTTP throughput, and memory pool benchmarks (#4)
- Edge case tests and reorganized test directory (#3)
- GitHub Actions CI with build, test, and ASan checks (#1)

### Changed
- Replace heap-allocated vectors with fixed buffers in retransmit/OOO queues (#5)

---

## [1.0.0] - 2025-02-05

### Added
- Complete user-space TCP/IP stack (IPv4, ICMP, UDP, TCP, HTTP 1.1, DNS)
- AI congestion control: Orca (SAC), bandwidth prediction (LSTM), anomaly detection (Autoencoder)
- NetworkAgent decision layer coordinating three models
- Cross-platform HAL: macOS (utun), Linux (TUN/TAP), Windows (Wintun)
- Streaming HTTP responses and data collection scripts
