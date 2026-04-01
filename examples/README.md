# NeuStack Examples

All examples live in `examples/` and link against the `neustack_lib` static library.

## Prerequisites

```bash
# Build all examples (from project root)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Or build a single example
cmake --build build --target echo_server
```

Most examples create a virtual network device (utun on macOS, TUN on Linux) and need **root**. After starting an example, run the NAT setup script once so the host can reach the stack IP:

```bash
# Terminal 1 — start the example
sudo ./build/examples/minimal

# Terminal 2 — set up NAT (replace utunX with the device printed at startup)
sudo ./scripts/nat/setup_nat.sh --dev utunX
```

---

## Examples

### minimal — HTTP Hello World

The simplest possible NeuStack program. Creates the stack, registers one GET route, and serves it on port 80.

```bash
sudo ./build/examples/minimal
# then: curl http://192.168.100.2/
```

**Key APIs:** `NeuStack::create()`, `http_server().get()`, `http_server().listen()`, `run()`

---

### echo_server — TCP Echo

Listens on TCP port 7000 and echoes back everything the client sends.

```bash
sudo ./build/examples/echo_server
# then: nc 192.168.100.2 7000
```

**Key APIs:** `tcp().listen()`, `IStreamConnection`, `StreamCallbacks`

---

### api_server — JSON REST API

Multi-route HTTP server with query parameters, POST body parsing, and auto-registered telemetry endpoints.

```bash
sudo ./build/examples/api_server
# then:
#   curl http://192.168.100.2/api/status
#   curl http://192.168.100.2/api/greet?name=World
#   curl -X POST -d '{"msg":"hello"}' http://192.168.100.2/api/echo
#   curl http://192.168.100.2/api/v1/stats | python3 -m json.tool
#   curl http://192.168.100.2/metrics
```

**Key APIs:** `http_server().get()/.post()`, `HttpRequest::query_param()`, `HttpRequest::body`

---

### dns_example — Async DNS Resolution

Resolves domain names asynchronously using NeuStack's built-in DNS client over UDP.

```bash
sudo ./build/examples/dns_example
```

**Key APIs:** `dns()->resolve_async()`, `DNSResponse::get_ip()`

---

### ping_example — ICMP Ping

Sends ICMP echo requests and measures round-trip time with sequence tracking.

```bash
sudo ./build/examples/ping_example
```

**Key APIs:** `icmp()->send_echo_request()`, `icmp()->set_echo_reply_callback()`

---

### fw_example — Firewall Rules

Demonstrates IP blacklisting, port-based rules, rate limiting, and shadow mode.

```bash
sudo ./build/examples/fw_example
```

**Key APIs:** `firewall_rules()->add_blacklist_ip()`, `firewall_rules()->add_rule()`, `firewall_rules()->rate_limiter()`

---

### http_client — Outbound HTTP GET

Makes an async HTTP GET request to an external server through the stack.

```bash
sudo ./build/examples/http_client
```

**Key APIs:** `http_client().get()`, async response callback

---

### telemetry_example — Prometheus & JSON Metrics

Serves built-in telemetry endpoints. NeuStack auto-registers `/api/v1/stats`, `/api/v1/health`, `/metrics` (Prometheus format), etc.

```bash
sudo ./build/examples/telemetry_example
# then:
#   curl http://192.168.100.2/api/v1/health
#   curl http://192.168.100.2/api/v1/stats | python3 -m json.tool
#   curl http://192.168.100.2/metrics
```

**Key APIs:** auto-registered by `NeuStack::create()`, `TelemetryAPI`

---

### udp_example — UDP Echo Server

Binds to a UDP port and echoes received datagrams with a prefix.

```bash
sudo ./build/examples/udp_example
```

**Key APIs:** `udp()->bind()`, `udp()->sendto()`

---

### afxdp_test — AF_XDP Packet Path (Linux only)

Tests the AF_XDP high-performance data path with ICMP auto-reply. Requires Linux with a compatible NIC.

```bash
# Build with AF_XDP support
cmake -B build -DNEUSTACK_ENABLE_AF_XDP=ON
cmake --build build --target afxdp_test
sudo ./build/examples/afxdp_test --iface eth0
```

**Build flags:** `-DNEUSTACK_ENABLE_AF_XDP=ON` (Linux only, requires libbpf)

---

### neustack_demo — Full Interactive Demo

The comprehensive demo combining all features: HTTP server, TCP streams, UDP, DNS, ICMP ping, firewall, telemetry, and optional AI agent. Includes a diagnostic UI and verbose logging.

```bash
sudo ./build/examples/neustack_demo --ip 192.168.100.2 -v

# With AI (requires -DNEUSTACK_ENABLE_AI=ON)
sudo ./build/examples/neustack_demo --ip 192.168.100.2 --ai -v
```

---

## Common Patterns

All examples follow the same structure:

```cpp
#include "neustack/neustack.hpp"
using namespace neustack;

int main() {
    StackConfig cfg;
    cfg.local_ip = "192.168.100.2";
    // ... configure as needed

    auto stack = NeuStack::create(cfg);
    if (!stack) return 1;

    // ... register handlers / routes

    stack->run();  // blocks until Ctrl+C
}
```

The stack IP defaults to `192.168.100.2` with gateway `192.168.100.1`. Adjust `StackConfig` fields to change these.
