# 教程 11: HTTP 客户端

## 概述

上一教程我们实现了 HTTP 服务器，能够接收请求并返回响应。本教程将实现 HTTP 客户端，让协议栈能够主动发起 HTTP 请求，访问外部 API。

HTTP 客户端与服务器的角色对比：

| | HTTP Server | HTTP Client |
|---|---|---|
| **角色** | 被动等待请求 | 主动发起请求 |
| **接收** | HTTP Request | HTTP Response |
| **发送** | HTTP Response | HTTP Request |
| **TCP 角色** | listen + accept | connect |

> **设计说明**：HTTP 客户端同样依赖 `IStreamClient` 抽象接口，
> 可以运行在 TCP、TLS 等不同传输层上。

## HTTP 请求/响应序列化

### 请求序列化

客户端需要将 `HttpRequest` 序列化为字节流发送：

```cpp
// 在 http_types.hpp 中添加
struct HttpRequest {
    // ... 原有字段 ...

    // 序列化为字节流（客户端发送用）
    std::string serialize() const {
        std::string result;

        // 请求行: METHOD PATH VERSION
        result += http_method_name(method);
        result += " ";
        result += path;
        result += " ";
        result += version.empty() ? "HTTP/1.1" : version;
        result += "\r\n";

        // 请求头
        for (const auto& [key, values] : headers) {
            for (const auto& value : values) {
                result += key;
                result += ": ";
                result += value;
                result += "\r\n";
            }
        }

        // 空行
        result += "\r\n";

        // 请求体
        result += body;

        return result;
    }
};
```

### 响应解析器

客户端需要解析收到的 HTTP 响应，与请求解析器类似：

```cpp
// include/neustack/app/http_response_parser.hpp
#ifndef NEUSTACK_APP_HTTP_RESPONSE_PARSER_HPP
#define NEUSTACK_APP_HTTP_RESPONSE_PARSER_HPP

#include "http_types.hpp"
#include <optional>

namespace neustack {

/**
 * HTTP 响应解析器
 *
 * 流式解析，处理分片到达的数据
 */
class HttpResponseParser {
public:
    enum class State {
        StatusLine,     // 解析状态行
        Headers,        // 解析头部
        Body,           // 解析响应体
        Complete,       // 解析完成
        Error           // 解析错误
    };

    HttpResponseParser() = default;

    // 喂入数据
    size_t feed(const uint8_t* data, size_t len);
    size_t feed(const std::string& data) {
        return feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    // 状态查询
    State state() const { return _state; }
    bool is_complete() const { return _state == State::Complete; }
    bool has_error() const { return _state == State::Error; }

    // 获取结果
    const HttpResponse& response() const { return _response; }
    HttpResponse take_response() { return std::move(_response); }

    // 重置
    void reset();

    // 错误信息
    const std::string& error() const { return _error; }

private:
    State _state = State::StatusLine;
    HttpResponse _response;
    std::string _buffer;
    std::string _error;
    size_t _content_length = 0;
    bool _chunked = false;

    bool parse_status_line();
    bool parse_headers();
    bool parse_body();
};

} // namespace neustack

#endif
```

### 响应解析器实现

```cpp
// src/app/http_response_parser.cpp
#include "neustack/app/http_response_parser.hpp"
#include <algorithm>

using namespace neustack;

size_t HttpResponseParser::feed(const uint8_t* data, size_t len) {
    if (_state == State::Complete || _state == State::Error) {
        return 0;
    }

    _buffer.append(reinterpret_cast<const char*>(data), len);

    while (_state != State::Complete && _state != State::Error) {
        switch (_state) {
            case State::StatusLine:
                if (!parse_status_line()) return len;
                break;
            case State::Headers:
                if (!parse_headers()) return len;
                break;
            case State::Body:
                if (!parse_body()) return len;
                break;
            default:
                break;
        }
    }

    return len;
}

bool HttpResponseParser::parse_status_line() {
    // 查找 \r\n
    auto pos = _buffer.find("\r\n");
    if (pos == std::string::npos) {
        return false;  // 数据不完整
    }

    std::string line = _buffer.substr(0, pos);
    _buffer.erase(0, pos + 2);

    // 解析: HTTP/1.1 200 OK
    auto sp1 = line.find(' ');
    auto sp2 = line.find(' ', sp1 + 1);

    if (sp1 == std::string::npos) {
        _state = State::Error;
        _error = "Invalid status line";
        return true;
    }

    // 版本
    std::string version = line.substr(0, sp1);

    // 状态码
    std::string status_str;
    if (sp2 != std::string::npos) {
        status_str = line.substr(sp1 + 1, sp2 - sp1 - 1);
    } else {
        status_str = line.substr(sp1 + 1);
    }

    int status_code = std::stoi(status_str);
    _response.status = static_cast<HttpStatus>(status_code);

    _state = State::Headers;
    return true;
}

bool HttpResponseParser::parse_headers() {
    while (true) {
        auto pos = _buffer.find("\r\n");
        if (pos == std::string::npos) {
            return false;
        }

        if (pos == 0) {
            // 空行，头部结束
            _buffer.erase(0, 2);

            // 检查 Content-Length
            std::string cl = _response.get_header("Content-Length");
            if (!cl.empty()) {
                _content_length = std::stoul(cl);
            }

            // 检查 Transfer-Encoding: chunked
            std::string te = _response.get_header("Transfer-Encoding");
            _chunked = (te.find("chunked") != std::string::npos);

            // 判断是否有 body
            if (_content_length > 0 || _chunked) {
                _state = State::Body;
            } else {
                _state = State::Complete;
            }
            return true;
        }

        std::string line = _buffer.substr(0, pos);
        _buffer.erase(0, pos + 2);

        // 解析 Key: Value
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            _state = State::Error;
            _error = "Invalid header line";
            return true;
        }

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // 去除前导空格
        while (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }

        _response.headers[key].push_back(value);
    }
}

bool HttpResponseParser::parse_body() {
    if (_chunked) {
        // 简化实现：暂不支持 chunked，等待完整数据
        // 实际应解析 chunk 格式
        _state = State::Error;
        _error = "Chunked transfer not implemented";
        return true;
    }

    if (_buffer.size() >= _content_length) {
        _response.body = _buffer.substr(0, _content_length);
        _buffer.erase(0, _content_length);
        _state = State::Complete;
        return true;
    }

    return false;  // 数据不完整
}

void HttpResponseParser::reset() {
    _state = State::StatusLine;
    _response = HttpResponse{};
    _buffer.clear();
    _error.clear();
    _content_length = 0;
    _chunked = false;
}
```

## HTTP 客户端实现

### 接口设计

```cpp
// include/neustack/app/http_client.hpp
#ifndef NEUSTACK_APP_HTTP_CLIENT_HPP
#define NEUSTACK_APP_HTTP_CLIENT_HPP

#include "http_types.hpp"
#include "http_response_parser.hpp"
#include "neustack/transport/stream.hpp"
#include <functional>
#include <memory>
#include <queue>

namespace neustack {

/**
 * HTTP 客户端
 *
 * 异步、非阻塞设计
 *
 * 使用示例:
 *   HttpClient client(tcp_layer);
 *
 *   client.get(server_ip, 80, "/api/data", [](const HttpResponse& resp) {
 *       if (resp.status == HttpStatus::OK) {
 *           printf("Response: %s\n", resp.body.c_str());
 *       }
 *   });
 */
class HttpClient {
public:
    // 响应回调
    using ResponseCallback = std::function<void(const HttpResponse& response, int error)>;

    // 依赖 IStreamClient 接口
    explicit HttpClient(IStreamClient& transport) : _transport(transport) {}

    /**
     * @brief 发起 GET 请求
     * @param server_ip 服务器 IP
     * @param port 端口
     * @param path 路径
     * @param on_response 响应回调 (error: 0=成功, -1=连接失败, -2=解析失败)
     */
    void get(uint32_t server_ip, uint16_t port, const std::string& path,
             ResponseCallback on_response);

    /**
     * @brief 发起 POST 请求
     */
    void post(uint32_t server_ip, uint16_t port, const std::string& path,
              const std::string& body, const std::string& content_type,
              ResponseCallback on_response);

    /**
     * @brief 发起自定义请求
     */
    void request(uint32_t server_ip, uint16_t port,
                 const HttpRequest& req, ResponseCallback on_response);

    /**
     * @brief 设置默认 Host 头
     */
    void set_default_host(const std::string& host) { _default_host = host; }

    /**
     * @brief 设置默认 User-Agent
     */
    void set_user_agent(const std::string& ua) { _user_agent = ua; }

private:
    IStreamClient& _transport;
    std::string _default_host;
    std::string _user_agent = "NeuStack/1.0";

    // 请求上下文
    struct RequestContext {
        HttpResponseParser parser;
        ResponseCallback callback;
        IStreamConnection* conn = nullptr;
    };
};

} // namespace neustack

#endif
```

### 客户端实现

```cpp
// src/app/http_client.cpp
#include "neustack/app/http_client.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

using namespace neustack;

void HttpClient::get(uint32_t server_ip, uint16_t port, const std::string& path,
                     ResponseCallback on_response) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.path = path;
    request(server_ip, port, req, std::move(on_response));
}

void HttpClient::post(uint32_t server_ip, uint16_t port, const std::string& path,
                      const std::string& body, const std::string& content_type,
                      ResponseCallback on_response) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = path;
    req.body = body;
    req.add_header("Content-Type", content_type);
    req.add_header("Content-Length", std::to_string(body.size()));
    request(server_ip, port, req, std::move(on_response));
}

void HttpClient::request(uint32_t server_ip, uint16_t port,
                         const HttpRequest& req, ResponseCallback on_response) {
    // 创建请求上下文（使用 shared_ptr 在回调中保持生命周期）
    auto ctx = std::make_shared<RequestContext>();
    ctx->callback = std::move(on_response);

    // 构建完整请求
    HttpRequest full_req = req;

    // 添加必要的 headers
    if (!full_req.has_header("Host")) {
        if (!_default_host.empty()) {
            full_req.add_header("Host", _default_host);
        } else {
            full_req.add_header("Host", ip_to_string(server_ip));
        }
    }
    if (!full_req.has_header("User-Agent")) {
        full_req.add_header("User-Agent", _user_agent);
    }
    if (!full_req.has_header("Connection")) {
        full_req.add_header("Connection", "close");  // 简单起见，不复用连接
    }

    // 序列化请求
    std::string request_data = full_req.serialize();

    LOG_DEBUG(APP, "HTTP Client: connecting to %s:%u",
              ip_to_string(server_ip).c_str(), port);

    // 发起连接
    _transport.connect(
        server_ip, port,
        // on_connect
        [ctx, request_data](IStreamConnection* conn, int error) {
            if (error != 0 || conn == nullptr) {
                LOG_WARN(APP, "HTTP Client: connection failed");
                if (ctx->callback) {
                    ctx->callback(HttpResponse{}, -1);
                }
                return;
            }

            ctx->conn = conn;
            LOG_DEBUG(APP, "HTTP Client: connected, sending request");

            // 发送请求
            conn->send(reinterpret_cast<const uint8_t*>(request_data.data()),
                       request_data.size());
        },
        // on_receive
        [ctx](IStreamConnection* conn, const uint8_t* data, size_t len) {
            ctx->parser.feed(data, len);

            if (ctx->parser.is_complete()) {
                LOG_DEBUG(APP, "HTTP Client: response complete");
                if (ctx->callback) {
                    ctx->callback(ctx->parser.response(), 0);
                    ctx->callback = nullptr;  // 只调用一次
                }
                conn->close();
            } else if (ctx->parser.has_error()) {
                LOG_WARN(APP, "HTTP Client: parse error: %s",
                         ctx->parser.error().c_str());
                if (ctx->callback) {
                    ctx->callback(HttpResponse{}, -2);
                    ctx->callback = nullptr;
                }
                conn->close();
            }
        },
        // on_close
        [ctx](IStreamConnection* conn) {
            LOG_DEBUG(APP, "HTTP Client: connection closed");

            // 如果连接关闭时还没收到完整响应
            if (ctx->callback) {
                if (ctx->parser.state() != HttpResponseParser::State::StatusLine) {
                    // 部分响应，可能服务器没发 Content-Length
                    ctx->callback(ctx->parser.response(), 0);
                } else {
                    ctx->callback(HttpResponse{}, -1);
                }
                ctx->callback = nullptr;
            }
        }
    );
}
```

## TCPLayer 实现 IStreamClient

为了让 HTTP 客户端工作，`TCPLayer` 需要实现 `IStreamClient` 接口：

```cpp
// 在 tcp_layer.hpp 中
class TCPLayer : public IProtocolHandler, public IStreamServer, public IStreamClient {
public:
    // ... 原有接口 ...

    // IStreamClient 接口
    int connect(uint32_t remote_ip, uint16_t remote_port,
                ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close) override;
};
```

实现已在之前的 `tcp_layer.cpp` 中完成（`TCPLayer::connect` 方法）。

## 使用示例

### 简单 GET 请求

```cpp
#include "neustack/app/http_client.hpp"

int main() {
    // ... 初始化 HAL, IP, TCP ...

    HttpClient http(tcp_layer);

    // 设置默认 Host
    http.set_default_host("api.example.com");

    // 发起 GET 请求
    uint32_t server_ip = ip_from_string("93.184.216.34");  // example.com

    http.get(server_ip, 80, "/", [](const HttpResponse& resp, int error) {
        if (error != 0) {
            printf("Request failed: %d\n", error);
            return;
        }

        printf("Status: %d\n", static_cast<int>(resp.status));
        printf("Body length: %zu\n", resp.body.size());
        printf("Body: %.100s...\n", resp.body.c_str());
    });

    // 主循环（处理收包、定时器等）
    while (running) {
        // ... 收包处理 ...
        tcp_layer.on_timer();
    }
}
```

### POST 请求

```cpp
// POST JSON 数据
http.post(server_ip, 80, "/api/users",
    R"({"name": "Alice", "email": "alice@example.com"})",
    "application/json",
    [](const HttpResponse& resp, int error) {
        if (error == 0 && resp.status == HttpStatus::Created) {
            printf("User created!\n");
        }
    }
);
```

### 自定义请求

```cpp
// 自定义请求（带 Authorization）
HttpRequest req;
req.method = HttpMethod::GET;
req.path = "/api/protected";
req.add_header("Authorization", "Bearer my_token_here");
req.add_header("Accept", "application/json");

http.request(server_ip, 80, req, [](const HttpResponse& resp, int error) {
    if (resp.status == HttpStatus::Unauthorized) {
        printf("Token expired!\n");
    }
});
```

### 与 DNS 配合使用

结合下一教程的 DNS 客户端，可以实现域名访问：

```cpp
// 先解析域名
dns.resolve("api.example.com", [&http](uint32_t ip, int error) {
    if (error != 0) {
        printf("DNS lookup failed\n");
        return;
    }

    // 再发起 HTTP 请求
    http.get(ip, 80, "/api/data", [](const HttpResponse& resp, int error) {
        // 处理响应
    });
});
```

## 文件结构

```
include/neustack/app/
├── http_types.hpp           # 类型定义（添加 Request::serialize）
├── http_parser.hpp          # 请求解析器（服务端用）
├── http_response_parser.hpp # 响应解析器（客户端用）
├── http_server.hpp          # HTTP 服务器
└── http_client.hpp          # HTTP 客户端

src/app/
├── http_types.cpp
├── http_parser.cpp
├── http_response_parser.cpp
├── http_server.cpp
└── http_client.cpp
```

## 扩展思考

### 连接复用

当前实现每次请求都建立新连接（`Connection: close`）。可以优化为：

```cpp
class HttpClient {
    // 连接池：server_ip:port -> connection
    std::unordered_map<uint64_t, IStreamConnection*> _pool;

    // Keep-Alive 支持
    void request(...) {
        auto key = make_pool_key(server_ip, port);
        auto conn = get_pooled_connection(key);
        if (conn) {
            // 复用连接
        } else {
            // 新建连接
        }
    }
};
```

### 超时处理

```cpp
class HttpClient {
    uint32_t _connect_timeout_ms = 5000;
    uint32_t _response_timeout_ms = 30000;

    // 使用定时器实现超时
    void request(...) {
        auto timer = start_timer(_connect_timeout_ms, [ctx]() {
            if (!ctx->connected) {
                ctx->callback(HttpResponse{}, -3);  // timeout
                ctx->conn->close();
            }
        });
    }
};
```

### 重定向跟随

```cpp
void HttpClient::request(..., int max_redirects = 5) {
    // 在回调中检查 301/302
    auto wrapped_callback = [this, max_redirects, on_response](
        const HttpResponse& resp, int error) {

        if (error == 0 && is_redirect(resp.status) && max_redirects > 0) {
            std::string location = resp.get_header("Location");
            // 解析新 URL，递归请求
            request(new_ip, new_port, new_req, on_response, max_redirects - 1);
        } else {
            on_response(resp, error);
        }
    };
}
```

## 小结

本教程实现了 HTTP 客户端，核心要点：

1. **请求序列化**：将 `HttpRequest` 转换为字节流发送
2. **响应解析**：流式解析 HTTP Response
3. **异步设计**：基于回调的非阻塞 API
4. **传输抽象**：依赖 `IStreamClient` 接口

HTTP 客户端与服务器配合，让我们的协议栈具备了完整的 HTTP 通信能力。

下一教程我们将实现 DNS 客户端，实现域名解析功能。
