# Changelog

All notable changes to NeuStack will be documented in this file.

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
