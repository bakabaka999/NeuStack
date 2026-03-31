# NeuStack Core API Reference

This document describes the current public C++ facade exposed by `include/neustack/neustack.hpp`.

## Table of Contents

- [NeuStack Facade](#neustack-facade)
- [HTTP Server](#http-server)
- [HTTP Client](#http-client)
- [DNS Client](#dns-client)
- [TCP Layer](#tcp-layer)
- [Configuration Options](#configuration-options)
- [Utility Functions](#utility-functions)
- [Threading Model](#threading-model)

---

## NeuStack Facade

### Header File

```cpp
#include "neustack/neustack.hpp"
```

### Creating an Instance

```cpp
using namespace neustack;

// Default configuration
auto stack = NeuStack::create();

// Custom configuration
StackConfig config;
config.local_ip = "192.168.100.2";
config.dns_server = ip_from_string("8.8.8.8");
config.device_type = "tun";      // or "af_xdp" on Linux
config.device_ifname = "";       // NIC name for AF_XDP
config.firewall_shadow_mode = true;
config.orca_model_path = "models/orca_actor.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";
config.anomaly_model_path = "models/anomaly_detector.onnx";

auto stack = NeuStack::create(config);
if (!stack) {
    return 1;
}
```

### Main Facade Methods

```cpp
// Hardware / protocol layers
NetDevice& device();
IPv4Layer& ip();
ICMPHandler* icmp();   // nullptr when ICMP is disabled
UDPLayer* udp();       // nullptr when UDP is disabled
TCPLayer& tcp();

// Application layer
HttpServer& http_server();
HttpClient& http_client();
DNSClient* dns();      // nullptr when UDP is disabled

// Firewall facade
bool firewall_enabled() const;
bool firewall_ai_enabled() const;
bool firewall_shadow_mode() const;
void firewall_set_shadow_mode(bool shadow);
void firewall_set_threshold(float threshold);
bool firewall_inspect(const uint8_t* data, size_t len);
void firewall_on_timer();
RuleEngine* firewall_rules();
FirewallStats firewall_stats() const;
FirewallAIStats firewall_ai_stats() const;

// Telemetry
telemetry::TelemetryAPI& telemetry();
std::string status_json(bool pretty = false);
std::string status_prometheus();

// AI status
bool ai_enabled() const;

// Lifecycle
void run();
void stop();
bool running() const;
```

Notes:

- `dns()`, `udp()`, and `icmp()` return pointers because those subsystems can be disabled by config.
- AI model paths are runtime configuration only; actual inference support still requires building with `-DNEUSTACK_ENABLE_AI=ON`.

---

## HTTP Server

The built-in HTTP server runs on top of NeuStack's stream interface.

### Route Registration

```cpp
auto& server = stack->http_server();

server.get("/", [](const neustack::HttpRequest&) {
    return neustack::HttpResponse()
        .content_type("text/plain")
        .set_body("Hello from NeuStack!\n");
});

server.post("/api/echo", [](const neustack::HttpRequest& req) {
    return neustack::HttpResponse()
        .content_type("application/json")
        .set_body(req.body);
});

server.listen(80);
```

### `HttpRequest`

```cpp
struct HttpRequest {
    HttpMethod method;
    std::string path;
    std::string version;
    std::string raw_query;
    std::unordered_map<std::string, std::string> query_params;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    std::string get_header(const std::string& key) const;
    std::vector<std::string> get_headers(const std::string& key) const;
    bool has_header(const std::string& key) const;
    std::string query_param(const std::string& key) const;
};
```

### `HttpResponse`

```cpp
struct HttpResponse {
    HttpStatus status = HttpStatus::OK;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    HttpResponse& set_header(const std::string& key, const std::string& value);
    HttpResponse& add_header(const std::string& key, const std::string& value);
    HttpResponse& content_type(const std::string& type);
    HttpResponse& set_body(const std::string& content);
};
```

---

## HTTP Client

The HTTP client is asynchronous and uses NeuStack's TCP implementation underneath.

```cpp
auto& client = stack->http_client();
client.set_default_host("example.com");

client.get(neustack::ip_from_string("93.184.216.34"), 80, "/",
    [](const neustack::HttpResponse& resp, int error) {
        if (error != 0) {
            std::printf("HTTP error: %d\n", error);
            return;
        }

        std::printf("Response bytes: %zu\n", resp.body.size());
    }
);
```

### Main Methods

```cpp
void get(uint32_t server_ip, uint16_t port, const std::string& path,
         ResponseCallback on_response);

void post(uint32_t server_ip, uint16_t port, const std::string& path,
          const std::string& body, const std::string& content_type,
          ResponseCallback on_response);

void request(uint32_t server_ip, uint16_t port,
             const HttpRequest& req, ResponseCallback on_response);
```

---

## DNS Client

NeuStack's DNS client depends on UDP. Always check the pointer returned by `stack->dns()`.

```cpp
if (auto* dns = stack->dns()) {
    dns->set_server(neustack::ip_from_string("8.8.8.8"));

    dns->resolve_async("example.com", [](std::optional<neustack::DNSResponse> response) {
        if (!response) {
            std::puts("DNS lookup failed");
            return;
        }

        if (auto ip = response->get_ip()) {
            std::printf("Resolved: %s\n", neustack::ip_to_string(*ip).c_str());
        }
    });
}
```

### Main Methods

```cpp
bool init();
void set_server(uint32_t ip);
void resolve_async(const std::string& hostname, DNSCallback callback,
                   DNSType type = DNSType::A);
void on_timer();
```

---

## TCP Layer

The TCP layer exposes both server and client stream interfaces.

### Listening

```cpp
auto& tcp = stack->tcp();

tcp.listen(8080, [](neustack::IStreamConnection* conn) -> neustack::StreamCallbacks {
    (void)conn;
    return {
        .on_receive = [](neustack::IStreamConnection* c, const uint8_t* data, size_t len) {
            c->send(data, len);  // echo
        },
        .on_close = [](neustack::IStreamConnection*) {
        }
    };
});
```

### Connecting

```cpp
auto server_ip = neustack::ip_from_string("192.168.100.1");

stack->tcp().connect(
    server_ip, 8080,
    [](neustack::IStreamConnection* conn, int err) {
        if (err != 0) return;

        static const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o', '\n'};
        conn->send(hello, sizeof(hello));
    },
    [](neustack::IStreamConnection*, const uint8_t* data, size_t len) {
        std::printf("Received %zu bytes\n", len);
        (void)data;
    },
    [](neustack::IStreamConnection*) {
    }
);
```

### `IStreamConnection`

```cpp
class IStreamConnection {
public:
    virtual ssize_t send(const uint8_t* data, size_t len) = 0;
    virtual void close() = 0;
    virtual uint32_t remote_ip() const;
    virtual uint16_t remote_port() const;
};
```

---

## Configuration Options

### `StackConfig`

```cpp
struct StackConfig {
    std::string local_ip = "192.168.100.2";
    uint32_t dns_server = 0x08080808;
    LogLevel log_level = LogLevel::INFO;
    bool enable_icmp = true;
    bool enable_udp = true;

    std::string device_type = "tun";   // "tun" or "af_xdp"
    std::string device_ifname = "";
    int io_cpu = -1;

    bool enable_firewall = true;
    bool firewall_shadow_mode = true;

    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;
    std::string security_model_path;
    float security_threshold = 0.0f;

    std::string data_output_dir;
    int security_label = 0;
};
```

Key points:

- `device_type = "af_xdp"` is only meaningful on Linux builds with `-DNEUSTACK_ENABLE_AF_XDP=ON`.
- `security_threshold = 0.0f` means "read the threshold from model metadata".
- `data_output_dir` enables CSV exporters for TCP, metrics, and security data.

---

## Utility Functions

### IP Address Conversion

```cpp
#include "neustack/common/ip_addr.hpp"

uint32_t ip = neustack::ip_from_string("192.168.1.1");
std::string text = neustack::ip_to_string(ip);
```

### Logging

```cpp
#include "neustack/common/log.hpp"

LOG_INFO(APP, "stack started");
LOG_WARN(FW, "firewall anomaly detected");
LOG_ERROR(AI, "model load failed");
```

---

## Threading Model

NeuStack is designed around a single data-plane event loop. Treat the facade and its subcomponents as **not thread-safe** unless you add your own external synchronization.

Recommended pattern:

```cpp
auto stack = neustack::NeuStack::create();
if (!stack) return 1;

stack->run();  // blocks until stop() or process termination
```
