# Telemetry & Observability API Reference

NeuStack provides a built-in telemetry framework for runtime observability, supporting Prometheus scraping, JSON API queries, and a standalone CLI monitoring tool.

## Table of Contents

- [Overview](#overview)
- [HTTP Endpoints](#http-endpoints)
- [TelemetryAPI (C++)](#telemetryapi-c)
- [MetricsRegistry](#metricsregistry)
- [Exporters](#exporters)
- [neustack-stat CLI Tool](#neustack-stat-cli-tool)
- [Usage Examples](#usage-examples)

---

## Overview

### Architecture

```
┌─ Data Plane (hot path, zero-allocation) ──────────────┐
│   GlobalMetrics    SecurityMetrics    TCPSample        │
│      (atomic counters, relaxed ordering)               │
└───────────────────────┬────────────────────────────────┘
                        │ bridge (read-only)
┌───────────────────────▼────────────────────────────────┐
│                 MetricsRegistry                         │
│         Counter · Gauge · Histogram                     │
└──────┬──────────────┬──────────────┬───────────────────┘
       │              │              │
┌──────▼─────┐ ┌──────▼─────┐ ┌─────▼──────────────┐
│JsonExporter│ │ Prometheus │ │   TelemetryAPI     │
│            │ │  Exporter  │ │ (C++ query iface)  │
└──────┬─────┘ └──────┬─────┘ └─────┬──────────────┘
       │              │              │
┌──────▼──────────────▼──────────────▼───────────────────┐
│              HttpServer Routes                          │
│  /api/v1/stats  /api/v1/connections  /metrics           │
└────────────────────────┬───────────────────────────────┘
                         │ HTTP
                ┌────────▼────────┐
                │  neustack-stat  │
                │  (standalone)   │
                └─────────────────┘
```

### Design Principles

- **Zero data-plane overhead** — existing atomic counters unchanged, telemetry reads on-demand
- **No external dependencies** — JSON built manually, no protobuf/nlohmann
- **Pluggable exporters** — JSON, Prometheus, future: OpenTelemetry, StatsD
- **Cross-platform** — CLI tool works on macOS, Linux, Windows

---

## HTTP Endpoints

All endpoints are auto-registered on stack startup. No manual route setup required.

| Endpoint | Format | Description |
|----------|--------|-------------|
| `GET /api/v1/health` | JSON | Health check (uptime, version) |
| `GET /api/v1/stats` | JSON | Complete stack snapshot (traffic, TCP, security, AI) |
| `GET /api/v1/stats/traffic` | JSON | Traffic counters only (packets, bytes, PPS, BPS) |
| `GET /api/v1/stats/tcp` | JSON | TCP stats only (connections, RTT, CWND) |
| `GET /api/v1/stats/security` | JSON | Firewall & AI stats only |
| `GET /api/v1/connections` | JSON | Active TCP connection list with per-connection details |
| `GET /metrics` | Prometheus | Prometheus exposition format |

### Query Parameters

- `?pretty=true` — Pretty-print JSON output (default: compact)

### Examples

```bash
# Full JSON snapshot
curl http://192.168.100.2:8080/api/v1/stats?pretty=true

# Prometheus scraping
curl http://192.168.100.2:8080/metrics

# Active connections
curl http://192.168.100.2:8080/api/v1/connections?pretty=true
```

### JSON Response Structure (`/api/v1/stats`)

```json
{
  "uptime_seconds": 42,
  "traffic": {
    "rx_packets": 15832,
    "tx_packets": 14201,
    "rx_bytes": 12456000,
    "tx_bytes": 11023000,
    "pps_rx": 1583.2,
    "pps_tx": 1420.1,
    "bps_rx": 1245600.0,
    "bps_tx": 1102300.0
  },
  "tcp": {
    "active_connections": 3,
    "rtt_avg_us": 1200,
    "rtt_p50_us": 1100,
    "rtt_p99_us": 3500
  },
  "security": {
    "firewall_enabled": true,
    "shadow_mode": true,
    "ai_anomaly_score": 0.0012,
    "ai_state": "NORMAL"
  }
}
```

---

## TelemetryAPI (C++)

The `TelemetryAPI` class provides programmatic access to stack metrics.

```cpp
#include "neustack/telemetry/telemetry_api.hpp"

// Access via NeuStack facade
auto& telemetry = stack->telemetry();

// Get full status snapshot
auto status = telemetry.status();
printf("PPS: %.1f\n", status.traffic.pps_rx);
printf("Active TCP: %u\n", status.tcp.active_connections);
printf("AI State: %s\n", status.security.agent_state.c_str());

// Get as JSON string
std::string json = telemetry.to_json();
std::string json_pretty = telemetry.to_json(true);  // pretty-print

// Get as Prometheus format
std::string prom = telemetry.to_prometheus();
```

### StackStatus Fields

| Category | Fields |
|----------|--------|
| **Traffic** | `packets_rx`, `packets_tx`, `bytes_rx`, `bytes_tx`, `pps_rx`, `pps_tx`, `bps_rx`, `bps_tx` |
| **TCP** | `active_connections`, `total_established`, `total_reset`, `total_timeout`, `rtt.avg_us`, `rtt.p50_us`, `rtt.p90_us`, `rtt.p99_us`, `avg_cwnd`, `total_retransmits` |
| **Security** | `firewall_enabled`, `shadow_mode`, `ai_enabled`, `pps`, `syn_rate`, `anomaly_score`, `agent_state`, `packets_dropped`, `packets_alerted` |
| **AI** | `enabled`, `orca_status`, `anomaly_status`, `bandwidth_status`, `current_alpha` |

---

## MetricsRegistry

Zero-allocation metric types for hot-path performance.

```cpp
#include "neustack/telemetry/metric_types.hpp"
#include "neustack/telemetry/metrics_registry.hpp"

// Metric types
Counter counter;     // Monotonically increasing (atomic)
counter.increment(); // lock-free
counter.add(10);

Gauge gauge;         // Can increase or decrease
gauge.set(42.0);
gauge.increment();
gauge.decrement();

Histogram hist;      // Distribution tracking
hist.observe(1200);  // Record a value (e.g., RTT in microseconds)
hist.percentile(50); // p50
hist.percentile(99); // p99
```

### Registry

The `MetricsRegistry` bridges existing metric sources (`GlobalMetrics`, `SecurityMetrics`) into the telemetry framework without modifying their interfaces.

---

## Exporters

### JsonExporter

```cpp
#include "neustack/telemetry/json_exporter.hpp"

JsonExporter exporter;
std::string json = exporter.serialize(registry);
std::string pretty = exporter.serialize(registry, true);
```

### PrometheusExporter

```cpp
#include "neustack/telemetry/prometheus_exporter.hpp"

PrometheusExporter exporter;
std::string text = exporter.serialize(registry);
// Output: neustack_rx_packets_total 15832
//         neustack_tcp_rtt_us{quantile="0.5"} 1100
//         ...
```

---

## neustack-stat CLI Tool

Standalone remote monitoring tool. Connects to NeuStack's HTTP API and renders a live terminal dashboard.

### Usage

```bash
# One-time snapshot
./neustack-stat --host 192.168.100.2

# Live dashboard (refreshes every second)
./neustack-stat --host 192.168.100.2 --live

# List active connections
./neustack-stat --host 192.168.100.2 connections

# Raw JSON output (for scripting)
./neustack-stat --host 192.168.100.2 --json
```

### Options

| Flag | Description |
|------|-------------|
| `--host <addr>` | Target NeuStack host (default: `127.0.0.1`) |
| `--port <port>` | Target port (default: `8080`) |
| `--live` / `-l` | Live refresh mode |
| `--interval <sec>` / `-i <sec>` | Refresh interval (default: `1`) |
| `--json` / `-j` | Raw JSON output |
| `--no-color` | Disable ANSI colors |
| `connections` | List active TCP connections |

### Live Dashboard Output

```
═══════════════════ NeuStack Status ═══════════════════
  Uptime: 42s    State: NORMAL

  Traffic
    RX: 15,832 pkts  12.5 MB   1,583 pps  1.2 MB/s
    TX: 14,201 pkts  11.0 MB   1,420 pps  1.1 MB/s

  TCP
    Active: 3 connections
    RTT:  avg=1.2ms  p50=1.1ms  p99=3.5ms

  Security
    Firewall: ON (Shadow Mode)
    AI Score: 0.0012   Anomalies: 0
═══════════════════════════════════════════════════════
```

---

## Usage Examples

### Prometheus + Grafana Setup

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'neustack'
    scrape_interval: 1s
    static_configs:
      - targets: ['192.168.100.2:8080']
    metrics_path: '/metrics'
```

### Programmatic Monitoring

```cpp
auto stack = neustack::NeuStack::create();

// HTTP endpoints are auto-registered
stack->http_server().listen(80);

// Programmatic access
auto& telemetry = stack->telemetry();

stack->on_timer([&]() {
    auto status = telemetry.status();
    if (status.traffic.pps_rx > 10000) {
        printf("High traffic: %.0f pps\n", status.traffic.pps_rx);
    }
});

stack->run();
```
