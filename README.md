<p align="center">
  <h1 align="center">NeuStack</h1>
  <p align="center">
    <strong>A cross-platform user-space TCP/IP stack built from scratch, with AI-powered congestion control and intelligent firewall</strong>
  </p>
  <p align="center">
    <a href="https://github.com/bakabaka999/NeuStack/actions"><img src="https://github.com/bakabaka999/NeuStack/workflows/CI/badge.svg" alt="CI"></a>
    <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"></a>
    <a href="https://pytorch.org/"><img src="https://img.shields.io/badge/PyTorch-SAC%20%2F%20LSTM%20%2F%20AE-EE4C2C.svg" alt="PyTorch"></a>
    <a href="https://onnxruntime.ai/"><img src="https://img.shields.io/badge/ONNX%20Runtime-inference-7B68EE.svg" alt="ONNX Runtime"></a>
    <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey.svg" alt="Platform">
    <img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License">
  </p>
</p>

**[中文](README.zh-CN.md) | English**

---

## 📖 Table of Contents

- [Introduction](#introduction)
- [Key Features](#key-features)
- [System Architecture](#system-architecture)
- [Quick Start](#quick-start)
- [Documentation](#documentation)
- [AI Intelligence Plane & NetworkAgent](#ai-intelligence-plane--networkagent)
- [Firewall](#firewall)
- [Testing](#testing)
- [Project Structure](#project-structure)
- [License](#license)

---

## Introduction

NeuStack is a **fully from-scratch** user-space TCP/IP stack with AI-driven congestion control and intelligent firewall capabilities.

- **C++ Core**: ~10,000 lines — complete protocol stack + AI inference + firewall
- **Python Training**: ~4,000 lines — SAC/LSTM/Autoencoder model training
- **Shell Scripts**: ~2,700 lines — data collection and environment setup

## Key Features

| Category | Details |
|----------|---------|
| **Protocol Stack** | IPv4 / ICMP / UDP / TCP / HTTP 1.1 / DNS |
| **Congestion Control** | Reno · CUBIC · **Orca (SAC Reinforcement Learning)** |
| **AI Intelligence Plane** | Bandwidth Prediction (LSTM) · Anomaly Detection (Autoencoder) · Smart Congestion Control (SAC) · Security Anomaly Detection (Deep AE) |
| **Firewall** | Blacklist/Whitelist · Port Blocking · Token Bucket Rate Limiting · AI Shadow Mode · Auto Escalation/De-escalation · Security Data Export |
| **NetworkAgent** | 4-state decision layer coordinating 4 models with policy-based clamp / fallback / connection control |
| **Cross-Platform** | macOS (utun) · Linux (TUN/TAP) · Windows (Wintun) |
| **Zero-Allocation Design** | FixedPool memory pool, no new/delete on hot paths |

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│              HTTP Server/Client  ·  DNS Client                │
├─────────────────────────────────────────────────────────────┤
│                    Transport Layer                            │
│          TCP (Reno / CUBIC / Orca)  ·  UDP                    │
├─────────────────────────────────────────────────────────────┤
│                    Network Layer                              │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              FirewallEngine                          │    │
│  │  ┌───────────┬──────────────┬───────────────────┐   │    │
│  │  │ Whitelist │  Blacklist   │  RateLimiter      │   │    │
│  │  │   (O(1))  │    (O(1))    │  (Token Bucket)   │   │    │
│  │  └───────────┴──────────────┴───────────────────┘   │    │
│  │  ┌─────────────────────────────────────────────┐    │    │
│  │  │ FirewallAI  (Deep AE · Shadow/Enforce Mode) │    │    │
│  │  └─────────────────────────────────────────────┘    │    │
│  └─────────────────────────────────────────────────────┘    │
│                     IPv4  ·  ICMP                            │
├─────────────────────────────────────────────────────────────┤
│               Hardware Abstraction Layer (HAL)                │
│           macOS utun  ·  Linux TUN  ·  Wintun                 │
└─────────────────────────────────────────────────────────────┘
                             ↕
              ┌──────────────────────────────────┐
              │     NetworkAgent (AI Decision)    │
              │  Orca (SAC) · BW Predict · Anomaly│
              └──────────────────────────────────┘
```

## Quick Start

### Dependencies

- CMake >= 3.20
- C++20 compiler (Clang >= 14 / GCC >= 11 / MSVC 2019+)
- Optional: ONNX Runtime (AI inference), Catch2 v3 (tests)

### Build

```bash
# Clone
git clone https://github.com/bakabaka999/NeuStack.git
cd NeuStack

# Build
cmake -B build -G Ninja
cmake --build build

# Test
cd build && ctest --output-on-failure

# Run (requires root)
sudo ./build/examples/neustack_demo
```

### Enable AI

```bash
./scripts/download_onnxruntime.sh
cmake -B build -DNEUSTACK_ENABLE_AI=ON
cmake --build build
```

### Minimal Example

```cpp
#include "neustack/neustack.hpp"

int main() {
    auto stack = neustack::NeuStack::create();

    // HTTP server
    stack->http_server().get("/", [](const auto&) {
        return neustack::HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(80);

    // Configure firewall (access rule engine via facade API)
    auto* rules = stack->firewall_rules();
    rules->add_blacklist_ip(0x01020304);  // Block 1.2.3.4
    rules->rate_limiter().set_enabled(true);
    rules->rate_limiter().set_rate(1000, 100);  // 1000 PPS

    stack->run();
}
```

## Documentation

| Document | Description |
|----------|-------------|
| [API Reference](docs/api/README.md) | C++ API reference |
| [Firewall Guide](docs/api/firewall.md) | Firewall configuration guide |
| [AI Training](docs/api/ai-training.md) | AI model training guide |
| [AI Inference](docs/api/ai-inference.md) | AI inference & NetworkAgent API |
| [Project Whitepaper](docs/project_whitepaper.md) | Project whitepaper (design details) |
| [Changelog](CHANGELOG.md) | Version history |
| [Contributing](CONTRIBUTING.md) | Contribution guide |

## AI Intelligence Plane & NetworkAgent

NeuStack's AI subsystem coordinates three models through **NetworkAgent**:

| Model | Algorithm | Purpose |
|-------|-----------|---------|
| **Orca** | SAC (Reinforcement Learning) | Smart congestion window adjustment |
| **Bandwidth Prediction** | LSTM | Proactive bandwidth estimation |
| **Security Anomaly Detection** | Deep Autoencoder | Firewall security threat detection |
| **Traffic Anomaly Detection** | Autoencoder | Detect abnormal network traffic patterns |

### NetworkAgent State Machine

```
NORMAL ──(BW drops sharply)──→ DEGRADED
   │                              │
   │(anomaly detected)       (anomaly detected)
   ↓                              ↓
UNDER_ATTACK ──(recovered)──→ RECOVERY ──→ NORMAL
```

See [AI Training Guide](docs/api/ai-training.md) for details.

## Security Model Training

```bash
# 1. Collect data (Docker environment)
cd docker && docker compose up -d
./scripts/linux/collect_security.sh

# 2. Clean & generate dataset
python training/security/data_clean.py --input data/ --output data/cleaned/
python scripts/csv_to_dataset.py --security data/cleaned/ --output data/security_dataset.npz

# 3. Train
python training/security/train.py --config training/security/config.yaml

# 4. Export ONNX
python training/security/export_onnx.py --checkpoint training/security/checkpoints/best.pt --output models/security_anomaly.onnx
```

## Docker Environment

```bash
cd docker
docker compose up -d    # Start TUN network environment
docker compose exec neustack bash
./build/examples/neustack_demo --models models/
```

## Firewall

NeuStack includes a built-in zero-allocation firewall engine with:

- **Blacklist/Whitelist**: O(1) hash lookup
- **Port Blocking**: Protocol filtering (TCP/UDP)
- **Token Bucket Rate Limiting**: Per-IP PPS limiting
- **Shadow Mode**: AI detection alert-only, with auto-escalation support
- **Security AI**: Deep Autoencoder 8-dimensional feature anomaly detection, ONNX inference

### Shadow Mode Auto-Escalation

```
Normal traffic ──→ Shadow Mode (alert only)
                       │
                 N consecutive anomalies
                       ↓
                Blocking Mode (block)
                       │
                 M consecutive normals
                       ↓
                Shadow Mode (recover)
```

Auto-escalation is enabled via `FirewallAIConfig::auto_escalate` (off by default). `escalate_cooldown_ms` controls the cooldown period to prevent oscillation.

```cpp
auto* rules = stack->firewall_rules();

// Whitelist (highest priority)
rules->add_whitelist_ip(ip_from_string("192.168.1.1"));

// Blacklist
rules->add_blacklist_ip(ip_from_string("1.2.3.4"));

// Block port
rules->add_rule(Rule::block_port(1, 22, 6));  // Block SSH (TCP)

// Rate limiting
rules->rate_limiter().set_enabled(true);
rules->rate_limiter().set_rate(1000, 100);  // 1000 PPS, burst 100

// View firewall stats
auto stats = stack->firewall_stats();
auto ai_stats = stack->firewall_ai_stats();
```

See [Firewall Guide](docs/api/firewall.md) for details.

## Testing

```bash
cd build
ctest --output-on-failure

# By category
ctest -R "unit"         # Unit tests
ctest -R "Integration"  # Integration tests
ctest -R "Benchmark"    # Benchmarks
```

| Category | Coverage |
|----------|----------|
| **Unit Tests** | Checksum, TCP/IP parsing, congestion control, HTTP parsing, firewall rule engine, rate limiter, security model |
| **Integration Tests** | TCP handshake, HTTP roundtrip, firewall packet filtering, AI Shadow Mode, Firewall E2E (16 cases / 91 assertions) |
| **Benchmarks** | Checksum throughput, queue performance, TCP throughput, memory pool performance |

## Project Structure

```
NeuStack/
├── include/neustack/          # Public headers
│   ├── common/                #   Common utilities
│   ├── hal/                   #   Hardware Abstraction Layer
│   ├── net/                   #   Network layer (IPv4, ICMP)
│   ├── transport/             #   Transport layer (TCP, UDP)
│   ├── app/                   #   Application layer (HTTP, DNS)
│   ├── firewall/              #   Firewall engine
│   ├── metrics/               #   Metrics collection
│   └── ai/                    #   AI inference
├── src/                       # Source implementations
├── tests/                     # Test code
├── training/                  # Python training code
│   ├── orca/                  #   SAC reinforcement learning
│   ├── bandwidth/             #   LSTM bandwidth prediction
│   ├── anomaly/               #   Traffic anomaly detection
│   └── security/              #   Security anomaly detection (Deep AE)
├── models/                    # ONNX models
├── scripts/                   # Shell scripts
│   ├── linux/                 #   Linux collection/traffic scripts
│   └── mac/                   #   macOS collection/traffic scripts
├── docker/                    # Docker TUN environment
├── examples/                  # Example programs
├── docs/                      # Documentation
│   ├── api/                   #   API reference
│   └── project_whitepaper.md  #   Whitepaper
└── cmake/                     # CMake modules
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `NEUSTACK_BUILD_TESTS` | ON | Build tests |
| `NEUSTACK_BUILD_EXAMPLES` | ON | Build examples |
| `NEUSTACK_ENABLE_ASAN` | OFF | Address Sanitizer |
| `NEUSTACK_ENABLE_AI` | OFF | AI congestion control + security anomaly detection |

## License

[MIT License](LICENSE)

---

<p align="center">
  <sub>Made with ❤️ by <a href="https://github.com/bakabaka999">bakabaka999</a></sub>
</p>
