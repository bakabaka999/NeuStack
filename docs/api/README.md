# NeuStack API Reference

This directory contains the API reference documentation for NeuStack.

## Contents

| Document | Description |
|----------|-------------|
| [Firewall](firewall.md) | Firewall engine API (rule engine, AI anomaly detection, Shadow Mode) |
| [AI Training](ai-training.md) | AI model training guide |
| [AI Inference](ai-inference.md) | AI inference engine & NetworkAgent decision layer |
| [Telemetry](telemetry.md) | Telemetry framework, HTTP endpoints, Prometheus export, CLI tool |
| [Integration](integration.md) | How to use NeuStack as a library in your project |
| [NeuStack Core](core.md) | Core API (protocol stack / HTTP / DNS) |
| [AF_XDP](af-xdp.md) | AF_XDP backend: NIC compatibility, modes, configuration, batch recv/send API |
| [Benchmark](benchmark.md) | Benchmark framework: micro-benchmarks, E2E throughput, results, reproduction |

## Quick Links

### Firewall

```cpp
#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/firewall/rule_engine.hpp"

// Main classes
FirewallEngine      // Firewall main engine
RuleEngine          // Rule management
RateLimiter         // Token bucket rate limiter
Rule                // Single rule
PacketEvent         // Parsed packet
FirewallDecision    // Firewall decision
FirewallAI          // AI anomaly detection layer
```

### Protocol Stack

```cpp
#include "neustack/neustack.hpp"

// Main classes
NeuStack            // Protocol stack main class (facade)
HttpServer          // HTTP server
HttpClient          // HTTP client
DNSClient           // DNS client
TCPLayer            // TCP layer
IPv4Layer           // IP layer
```

### Telemetry & Observability

```cpp
#include "neustack/telemetry/telemetry_api.hpp"

// Main classes
TelemetryAPI        // Real-time query interface
MetricsRegistry     // Metric registration center
JsonExporter        // JSON serialization
PrometheusExporter  // Prometheus exposition format
```

### NeuStack Facade API (Firewall)

```cpp
auto stack = NeuStack::create(config);

// Firewall operations
stack->firewall_rules();          // Access rule engine
stack->firewall_inspect(data, len); // Inspect packet
stack->firewall_stats();          // Firewall stats
stack->firewall_ai_stats();       // AI stats
stack->firewall_set_shadow_mode(true); // Toggle Shadow Mode
stack->firewall_set_threshold(0.01f);  // Adjust AI threshold
```

### AI Intelligence Plane

```cpp
#include "neustack/ai/ai_agent.hpp"
#include "neustack/ai/security_model.hpp"

// Main classes
NetworkAgent           // AI decision layer
OrcaModel              // SAC congestion control
BandwidthPredictor     // LSTM bandwidth prediction
AnomalyDetector        // Autoencoder anomaly detection (TCP side)
SecurityAnomalyModel   // Deep Autoencoder security anomaly detection (firewall side)
```

## Versions

- **v1.0**: Base protocol stack + AI congestion control
- **v1.1**: Performance optimization + test hardening
- **v1.2**: AI firewall + security training pipeline + E2E test suite
- **v1.3**: Telemetry framework + HTTP metrics endpoints + CLI monitoring tool + AI model hardening
- **v1.4**: AF_XDP high-performance backend (UMEM + BPF/XDP + batch ring I/O) + benchmark framework (micro + E2E throughput)
- **v1.5**: Exception-path correctness hardening: HTTP chunked support, ICMP error propagation, retransmit telemetry (current)
