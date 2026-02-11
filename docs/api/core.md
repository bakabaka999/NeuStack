# NeuStack Core API Reference

本文档介绍 NeuStack 核心协议栈 API。

## 目录

- [NeuStack](#neustack)
- [HTTP Server](#http-server)
- [HTTP Client](#http-client)
- [DNS Client](#dns-client)
- [TCP Layer](#tcp-layer)
- [配置选项](#配置选项)

---

## NeuStack

协议栈主类，提供统一的接口。

### 头文件

```cpp
#include "neustack/neustack.hpp"
```

### 创建实例

```cpp
// 默认配置
auto stack = NeuStack::create();

// 自定义配置
NeuStackConfig config;
config.local_ip = ip_from_string("192.168.100.2");
config.gateway_ip = ip_from_string("192.168.100.1");
config.device_name = "utun4";
auto stack = NeuStack::create(config);
```

### 主要方法

```cpp
// 获取各层组件
HttpServer& http_server();
HttpClient& http_client();
DNSClient& dns_client();
TCPLayer& tcp_layer();
IPv4Layer& ip_layer();
FirewallEngine& firewall();

// 运行事件循环
void run();                    // 阻塞运行
void run_once();               // 单次迭代
void stop();                   // 停止运行

// 定时器
void on_timer();               // 手动触发定时器
```

---

## HTTP Server

简单的 HTTP/1.1 服务器。

### 路由注册

```cpp
auto& server = stack->http_server();

// GET 请求
server.get("/", [](const HttpRequest& req) {
    return HttpResponse()
        .status(200)
        .content_type("text/html")
        .set_body("<h1>Hello!</h1>");
});

// POST 请求
server.post("/api/data", [](const HttpRequest& req) {
    auto body = req.body();
    // 处理数据...
    return HttpResponse()
        .status(201)
        .content_type("application/json")
        .set_body("{\"status\":\"ok\"}");
});

// 启动监听
server.listen(80);
```

### HttpRequest

```cpp
struct HttpRequest {
    HttpMethod method() const;
    const std::string& path() const;
    const std::string& body() const;
    std::optional<std::string> header(const std::string& name) const;
    std::optional<std::string> query(const std::string& name) const;
};
```

### HttpResponse

```cpp
class HttpResponse {
    HttpResponse& status(int code);
    HttpResponse& content_type(const std::string& type);
    HttpResponse& header(const std::string& name, const std::string& value);
    HttpResponse& set_body(const std::string& body);
    HttpResponse& set_body(const uint8_t* data, size_t len);
};
```

---

## HTTP Client

HTTP/1.1 客户端。

### 发起请求

```cpp
auto& client = stack->http_client();

// GET 请求
client.get("http://example.com/api/data",
    // on_response
    [](const HttpResponse& resp) {
        printf("Status: %d\n", resp.status_code());
        printf("Body: %s\n", resp.body().c_str());
    },
    // on_error
    [](int error) {
        printf("Error: %d\n", error);
    }
);

// POST 请求
client.post("http://example.com/api/data",
    "application/json",
    "{\"key\":\"value\"}",
    on_response,
    on_error
);
```

---

## DNS Client

异步 DNS 解析。

### 解析域名

```cpp
auto& dns = stack->dns_client();

// 设置 DNS 服务器
dns.set_server(ip_from_string("8.8.8.8"));

// 异步解析
dns.resolve("example.com",
    // on_success
    [](uint32_t ip) {
        printf("Resolved: %s\n", ip_to_string(ip).c_str());
    },
    // on_error
    [](int error) {
        printf("DNS error: %d\n", error);
    }
);
```

---

## TCP Layer

底层 TCP 接口。

### 监听

```cpp
auto& tcp = stack->tcp_layer();

tcp.listen(8080, [](IStreamConnection* conn) -> StreamCallbacks {
    return {
        .on_receive = [](IStreamConnection* c, const uint8_t* data, size_t len) {
            // 处理接收数据
            c->send(data, len);  // Echo
        },
        .on_close = [](IStreamConnection* c) {
            // 连接关闭
        }
    };
});
```

### 连接

```cpp
tcp.connect(server_ip, 8080,
    // on_connect
    [](IStreamConnection* conn, int err) {
        if (err == 0) {
            conn->send("Hello", 5);
        }
    },
    // on_receive
    [](IStreamConnection* conn, const uint8_t* data, size_t len) {
        // 处理数据
    },
    // on_close
    [](IStreamConnection* conn) {
        // 连接关闭
    }
);
```

### IStreamConnection

```cpp
class IStreamConnection {
    // 发送数据
    ssize_t send(const uint8_t* data, size_t len);
    ssize_t send(const char* data, size_t len);
    
    // 关闭连接
    void close();
    
    // 获取信息
    uint32_t remote_ip() const;
    uint16_t remote_port() const;
    uint32_t local_ip() const;
    uint16_t local_port() const;
};
```

---

## 配置选项

### NeuStackConfig

```cpp
struct NeuStackConfig {
    // 网络配置
    uint32_t local_ip = ip_from_string("192.168.100.2");
    uint32_t gateway_ip = ip_from_string("192.168.100.1");
    uint32_t netmask = ip_from_string("255.255.255.0");
    
    // 设备配置
    std::string device_name = "";  // 空 = 自动选择
    
    // TCP 配置
    size_t max_connections = 1024;
    uint32_t mss = 1460;
    
    // 拥塞控制
    CongestionControlType cc_type = CongestionControlType::CUBIC;
    
    // AI 配置 (需要 NEUSTACK_ENABLE_AI)
    bool enable_ai = false;
    std::string orca_model_path = "models/orca_actor.onnx";
    std::string bandwidth_model_path = "models/bandwidth_predictor.onnx";
    std::string anomaly_model_path = "models/anomaly_detector.onnx";
    
    // 防火墙配置
    bool firewall_enabled = true;
    bool firewall_shadow_mode = true;
};
```

### CongestionControlType

```cpp
enum class CongestionControlType {
    RENO,   // RFC 5681
    CUBIC,  // RFC 8312
    ORCA    // SAC 强化学习 (需要 AI)
};
```

---

## 工具函数

### IP 地址转换

```cpp
#include "neustack/common/ip_addr.hpp"

// 字符串 -> uint32_t
uint32_t ip = ip_from_string("192.168.1.1");

// uint32_t -> 字符串
std::string str = ip_to_string(ip);
```

### 日志

```cpp
#include "neustack/common/log.hpp"

// 设置日志级别
Logger::instance().set_level(LogLevel::DEBUG);

// 日志宏
LOG_DEBUG(APP, "Debug message: %d", value);
LOG_INFO(APP, "Info message");
LOG_WARN(APP, "Warning message");
LOG_ERROR(APP, "Error message");
```

---

## 错误码

| 错误码 | 含义 |
|--------|------|
| 0 | 成功 |
| -1 | 通用错误 |
| -2 | 连接被拒绝 |
| -3 | 连接超时 |
| -4 | 连接重置 |
| -5 | 网络不可达 |

---

## 线程安全

NeuStack **不是**线程安全的。所有操作应在单线程中进行，或使用外部同步机制。

推荐模式：
```cpp
// 主线程运行事件循环
stack->run();

// 或者手动迭代
while (running) {
    stack->run_once();
    stack->on_timer();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```
