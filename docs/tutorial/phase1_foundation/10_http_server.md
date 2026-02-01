# 教程 10: HTTP 协议与 Web 服务器

## 概述

经过前面的教程，我们已经实现了完整的 TCP/IP 协议栈。现在是时候在其上构建应用层协议了。HTTP（超文本传输协议）是互联网最重要的应用层协议，理解它的实现将帮助我们：

- 验证 TCP 栈的正确性
- 理解请求-响应模型
- 学习文本协议解析
- 为后续 HTTPS/TLS 打基础

> **设计说明**：HTTP 服务器依赖 `IStreamServer` 抽象接口，而非直接依赖 `TCPLayer`。
> 这样 HTTP 可以运行在任何实现了该接口的传输层上（TCP、TLS、QUIC 等）。
> 详见 [Extra 2: Stream 抽象接口](extra_02_stream_interface.md)。

## HTTP/1.1 协议基础

### 请求格式

```
GET /index.html HTTP/1.1\r\n
Host: example.com\r\n
User-Agent: Mozilla/5.0\r\n
Accept: text/html\r\n
\r\n
```

结构：
1. **请求行**: `方法 路径 HTTP版本\r\n`
2. **请求头**: `Key: Value\r\n` (可多行)
3. **空行**: `\r\n` (标志头部结束)
4. **请求体**: 可选 (POST/PUT 时使用)

### 响应格式

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 13\r\n
\r\n
Hello, World!
```

结构：
1. **状态行**: `HTTP版本 状态码 状态描述\r\n`
2. **响应头**: `Key: Value\r\n`
3. **空行**: `\r\n`
4. **响应体**: 实际内容

### 常见状态码

| 状态码 | 含义 |
|--------|------|
| 200 | OK - 请求成功 |
| 301 | Moved Permanently - 永久重定向 |
| 400 | Bad Request - 请求格式错误 |
| 404 | Not Found - 资源不存在 |
| 500 | Internal Server Error - 服务器错误 |

## 实现设计

### 文件结构

```
include/neustack/app/
├── http_parser.hpp     # HTTP 解析器
├── http_server.hpp     # HTTP 服务器
└── http_types.hpp      # 类型定义

src/app/
├── http_parser.cpp
└── http_server.cpp
```

### HTTP 类型定义

```cpp
// include/neustack/app/http_types.hpp
#ifndef NEUSTACK_APP_HTTP_TYPES_HPP
#define NEUSTACK_APP_HTTP_TYPES_HPP

#include <string>
#include <unordered_map>
#include <functional>

namespace neustack {

// HTTP 方法
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
    UNKNOWN
};

// HTTP 状态码
enum class HttpStatus {
    OK = 200,
    Created = 201,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503
};

// HTTP 请求
// 注意：headers 使用 vector 存储值，支持多值 Header（如多个 Cookie）
struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    // 获取单个 header 值（返回第一个）
    std::string get_header(const std::string& key) const {
        auto it = headers.find(key);
        return (it != headers.end() && !it->second.empty()) ? it->second[0] : "";
    }

    // 获取所有 header 值
    std::vector<std::string> get_headers(const std::string& key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : std::vector<std::string>{};
    }

    bool has_header(const std::string& key) const {
        return headers.count(key) > 0;
    }

    // 添加 header（支持多值）
    void add_header(const std::string& key, const std::string& value) {
        headers[key].push_back(value);
    }
};

// HTTP 响应
// 注意：headers 使用 vector 存储值，支持多值 Header（如多个 Set-Cookie）
struct HttpResponse {
    HttpStatus status = HttpStatus::OK;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    // 设置响应头（覆盖）
    HttpResponse& set_header(const std::string& key, const std::string& value) {
        headers[key] = {value};
        return *this;
    }

    // 添加响应头（追加，用于 Set-Cookie 等）
    HttpResponse& add_header(const std::string& key, const std::string& value) {
        headers[key].push_back(value);
        return *this;
    }

    // 设置 Content-Type
    HttpResponse& content_type(const std::string& type) {
        return set_header("Content-Type", type);
    }

    // 设置响应体
    HttpResponse& set_body(const std::string& content) {
        body = content;
        set_header("Content-Length", std::to_string(content.size()));
        return *this;
    }

    // 序列化为字节流
    std::string serialize() const;
};

// 请求处理器
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// 辅助函数
const char* http_method_name(HttpMethod method);
HttpMethod parse_http_method(const std::string& method);
const char* http_status_text(HttpStatus status);

} // namespace neustack

#endif
```

### HTTP 解析器

HTTP 解析需要处理流式数据（TCP 可能分多次发送一个请求）：

```cpp
// include/neustack/app/http_parser.hpp
#ifndef NEUSTACK_APP_HTTP_PARSER_HPP
#define NEUSTACK_APP_HTTP_PARSER_HPP

#include "http_types.hpp"
#include <optional>

namespace neustack {

/**
 * HTTP 请求解析器
 *
 * 特点：
 * - 流式解析：数据可以分多次喂入
 * - 状态机设计：处理不完整的请求
 * - 零拷贝优化：直接在缓冲区上解析
 */
class HttpParser {
public:
    enum class State {
        RequestLine,    // 解析请求行
        Headers,        // 解析头部
        Body,           // 解析请求体
        Complete,       // 解析完成
        Error           // 解析错误
    };

    HttpParser() = default;

    // 喂入数据，返回消耗的字节数
    size_t feed(const uint8_t* data, size_t len);
    size_t feed(const std::string& data) {
        return feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    // 获取解析状态
    State state() const { return _state; }
    bool is_complete() const { return _state == State::Complete; }
    bool has_error() const { return _state == State::Error; }

    // 获取解析结果
    const HttpRequest& request() const { return _request; }
    HttpRequest take_request() { return std::move(_request); }

    // 重置解析器（用于下一个请求）
    void reset();

    // 错误信息
    const std::string& error() const { return _error; }

private:
    State _state = State::RequestLine;
    HttpRequest _request;
    std::string _buffer;
    std::string _error;
    size_t _content_length = 0;

    bool parse_request_line();
    bool parse_headers();
    bool parse_body();
};

} // namespace neustack

#endif
```

### 解析器实现

```cpp
// src/app/http_parser.cpp
#include "neustack/app/http_parser.hpp"
#include <algorithm>

using namespace neustack;

size_t HttpParser::feed(const uint8_t* data, size_t len) {
    if (_state == State::Complete || _state == State::Error) {
        return 0;
    }

    // 追加到缓冲区
    _buffer.append(reinterpret_cast<const char*>(data), len);

    // 状态机处理
    while (_state != State::Complete && _state != State::Error) {
        switch (_state) {
            case State::RequestLine:
                if (!parse_request_line()) return len;
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

bool HttpParser::parse_request_line() {
    // 查找 \r\n
    auto pos = _buffer.find("\r\n");
    if (pos == std::string::npos) {
        return false;  // 数据不完整
    }

    std::string line = _buffer.substr(0, pos);
    _buffer.erase(0, pos + 2);

    // 解析: METHOD PATH VERSION
    auto sp1 = line.find(' ');
    auto sp2 = line.find(' ', sp1 + 1);

    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        _state = State::Error;
        _error = "Invalid request line";
        return true;
    }

    _request.method = parse_http_method(line.substr(0, sp1));
    _request.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    _request.version = line.substr(sp2 + 1);

    _state = State::Headers;
    return true;
}

bool HttpParser::parse_headers() {
    while (true) {
        auto pos = _buffer.find("\r\n");
        if (pos == std::string::npos) {
            return false;  // 数据不完整
        }

        if (pos == 0) {
            // 空行，头部结束
            _buffer.erase(0, 2);

            // 检查是否有请求体
            auto cl = _request.get_header("Content-Length");
            if (!cl.empty()) {
                _content_length = std::stoul(cl);
                _state = State::Body;
            } else {
                _state = State::Complete;
            }
            return true;
        }

        std::string line = _buffer.substr(0, pos);
        _buffer.erase(0, pos + 2);

        // 解析: Key: Value
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

        _request.headers[key].push_back(value);
    }
}

bool HttpParser::parse_body() {
    if (_buffer.size() >= _content_length) {
        _request.body = _buffer.substr(0, _content_length);
        _buffer.erase(0, _content_length);
        _state = State::Complete;
        return true;
    }
    return false;  // 数据不完整
}

void HttpParser::reset() {
    _state = State::RequestLine;
    _request = HttpRequest{};
    _buffer.clear();
    _error.clear();
    _content_length = 0;
}
```

### HTTP 服务器

```cpp
// include/neustack/app/http_server.hpp
#ifndef NEUSTACK_APP_HTTP_SERVER_HPP
#define NEUSTACK_APP_HTTP_SERVER_HPP

#include "http_types.hpp"
#include "http_parser.hpp"
#include "neustack/transport/stream.hpp"  // 使用抽象接口
#include <unordered_map>

namespace neustack {

/**
 * HTTP 服务器
 *
 * 依赖 IStreamServer 抽象接口，可运行在：
 * - TCP (HTTP/1.1)
 * - TLS (HTTPS)
 * - QUIC (HTTP/3)
 *
 * 使用示例:
 *   HttpServer server(tcp_layer);  // tcp_layer 实现了 IStreamServer
 *
 *   server.get("/", [](const HttpRequest& req) {
 *       return HttpResponse().set_body("Hello, World!");
 *   });
 *
 *   server.listen(80);
 */
class HttpServer {
public:
    // 依赖抽象接口，而非具体的 TCPLayer
    explicit HttpServer(IStreamServer& transport) : _transport(transport) {}

    // 注册路由
    void get(const std::string& path, HttpHandler handler);
    void post(const std::string& path, HttpHandler handler);
    void put(const std::string& path, HttpHandler handler);
    void del(const std::string& path, HttpHandler handler);

    // 注册静态文件目录
    void serve_static(const std::string& url_prefix, const std::string& dir_path);

    // 设置 404 处理器
    void set_not_found_handler(HttpHandler handler);

    // 启动监听
    int listen(uint16_t port);

private:
    IStreamServer& _transport;  // 抽象接口引用

    // 路由表: method -> path -> handler
    std::unordered_map<HttpMethod,
        std::unordered_map<std::string, HttpHandler>> _routes;

    HttpHandler _not_found_handler;
    std::string _static_prefix;
    std::string _static_dir;

    // 每个连接的状态
    struct ConnectionState {
        HttpParser parser;
        bool keep_alive = true;
    };
    std::unordered_map<IStreamConnection*, ConnectionState> _connections;

    // 处理连接
    void on_receive(IStreamConnection* conn, const uint8_t* data, size_t len);
    void on_close(IStreamConnection* conn);

    // 分发请求
    HttpResponse dispatch(const HttpRequest& request);

    // 发送响应
    void send_response(IStreamConnection* conn, const HttpResponse& response);
};

} // namespace neustack

#endif
```

### 服务器实现

```cpp
// src/app/http_server.cpp
#include "neustack/app/http_server.hpp"
#include "neustack/common/log.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace {

// MIME 类型映射表
const std::unordered_map<std::string, std::string> MIME_TYPES = {
    // 文本
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".xml",  "application/xml"},
    {".txt",  "text/plain"},

    // 图片
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".webp", "image/webp"},

    // 字体
    {".woff",  "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf",   "font/ttf"},

    // 其他
    {".pdf",  "application/pdf"},
    {".zip",  "application/zip"},
    {".wasm", "application/wasm"},
};

// 获取文件扩展名
std::string get_extension(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    return path.substr(pos);
}

// 根据扩展名获取 MIME 类型
const char* get_mime_type(const std::string& path) {
    auto it = MIME_TYPES.find(get_extension(path));
    return (it != MIME_TYPES.end()) ? it->second.c_str() : "application/octet-stream";
}

} // anonymous namespace

using namespace neustack;

void HttpServer::get(const std::string& path, HttpHandler handler) {
    _routes[HttpMethod::GET][path] = std::move(handler);
}

void HttpServer::post(const std::string& path, HttpHandler handler) {
    _routes[HttpMethod::POST][path] = std::move(handler);
}

void HttpServer::put(const std::string& path, HttpHandler handler) {
    _routes[HttpMethod::PUT][path] = std::move(handler);
}

void HttpServer::del(const std::string& path, HttpHandler handler) {
    _routes[HttpMethod::DELETE][path] = std::move(handler);
}

void HttpServer::set_not_found_handler(HttpHandler handler) {
    _not_found_handler = std::move(handler);
}

void HttpServer::serve_static(const std::string& url_prefix,
                               const std::string& dir_path) {
    _static_prefix = url_prefix;
    _static_dir = dir_path;
}

int HttpServer::listen(uint16_t port) {
    // 使用 IStreamServer 接口监听
    return _transport.listen(port, [this](IStreamConnection* conn) -> StreamCallbacks {
        // 新连接：初始化状态
        _connections[conn] = ConnectionState{};
        LOG_DEBUG(APP, "HTTP: new connection from %s:%u",
                  ip_to_string(conn->remote_ip()).c_str(), conn->remote_port());

        return StreamCallbacks{
            .on_receive = [this](IStreamConnection* conn, const uint8_t* data, size_t len) {
                on_receive(conn, data, len);
            },
            .on_close = [this](IStreamConnection* conn) {
                on_close(conn);
            }
        };
    });
}

void HttpServer::on_receive(IStreamConnection* conn, const uint8_t* data, size_t len) {
    auto it = _connections.find(conn);
    if (it == _connections.end()) return;

    auto& state = it->second;
    state.parser.feed(data, len);

    // 请求完整时处理
    while (state.parser.is_complete()) {
        HttpRequest request = state.parser.take_request();

        LOG_INFO(APP, "HTTP: %s %s",
                 http_method_name(request.method),
                 request.path.c_str());

        // 检查 Keep-Alive
        std::string conn_header = request.get_header("Connection");
        state.keep_alive = (conn_header != "close");

        // 分发并发送响应
        HttpResponse response = dispatch(request);

        // 设置 Connection 头
        if (!state.keep_alive) {
            response.set_header("Connection", "close");
        }

        send_response(conn, response);

        // 重置解析器以处理下一个请求
        state.parser.reset();

        // 非 Keep-Alive 则关闭连接
        if (!state.keep_alive) {
            conn->close();
            break;
        }
    }

    if (state.parser.has_error()) {
        LOG_WARN(APP, "HTTP: parse error: %s", state.parser.error().c_str());

        HttpResponse response;
        response.status = HttpStatus::BadRequest;
        response.set_body("Bad Request");
        send_response(conn, response);

        conn->close();
    }
}

void HttpServer::on_close(IStreamConnection* conn) {
    _connections.erase(conn);
    LOG_DEBUG(APP, "HTTP: connection closed");
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) {
    // 查找路由
    auto method_it = _routes.find(request.method);
    if (method_it != _routes.end()) {
        auto path_it = method_it->second.find(request.path);
        if (path_it != method_it->second.end()) {
            return path_it->second(request);
        }
    }

    // 尝试静态文件
    if (!_static_prefix.empty() &&
        request.path.find(_static_prefix) == 0 &&
        request.method == HttpMethod::GET) {

        std::string file_path = _static_dir +
            request.path.substr(_static_prefix.length());

        std::ifstream file(file_path, std::ios::binary);
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();

            HttpResponse response;
            response.set_body(buffer.str());
            response.content_type(get_mime_type(file_path));

            return response;
        }
    }

    // 404
    if (_not_found_handler) {
        return _not_found_handler(request);
    }

    HttpResponse response;
    response.status = HttpStatus::NotFound;
    response.content_type("text/html");
    response.set_body("<html><body><h1>404 Not Found</h1></body></html>");
    return response;
}

void HttpServer::send_response(IStreamConnection* conn, const HttpResponse& response) {
    std::string data = response.serialize();
    conn->send(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
```

## 使用示例

### 基础 Web 服务器

```cpp
#include "neustack/app/http_server.hpp"

int main() {
    // ... 初始化 HAL, IP, TCP ...

    // tcp_layer 实现了 IStreamServer
    HttpServer http(tcp_layer);

    // 首页
    http.get("/", [](const HttpRequest& req) {
        return HttpResponse()
            .content_type("text/html")
            .set_body("<html><body>"
                      "<h1>Welcome to NeuStack!</h1>"
                      "<p>A user-space TCP/IP stack.</p>"
                      "</body></html>");
    });

    // JSON API
    http.get("/api/status", [](const HttpRequest& req) {
        return HttpResponse()
            .content_type("application/json")
            .set_body("{\"status\": \"running\", \"version\": \"1.0\"}");
    });

    // POST 处理
    http.post("/api/echo", [](const HttpRequest& req) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body(req.body);
    });

    // 静态文件服务
    http.serve_static("/static/", "./www/");

    // 自定义 404
    http.set_not_found_handler([](const HttpRequest& req) {
        HttpResponse resp;
        resp.status = HttpStatus::NotFound;
        resp.content_type("text/html");
        resp.set_body("<h1>Page Not Found</h1>"
                      "<p>The page " + req.path + " does not exist.</p>");
        return resp;
    });

    http.listen(80);

    // 主循环...
}
```

### 测试

```bash
# 获取首页
curl http://192.168.100.2/

# 获取 JSON API
curl http://192.168.100.2/api/status

# POST 请求
curl -X POST -d "Hello" http://192.168.100.2/api/echo

# 使用浏览器访问
open http://192.168.100.2/
```

## 扩展思考

### 性能优化

1. **零拷贝发送**: 使用 `writev` 或 scatter-gather IO
2. **连接池**: 复用 TCB 对象
3. **静态文件缓存**: 避免重复读取磁盘

### 功能扩展

1. **分块传输**: `Transfer-Encoding: chunked`
2. **压缩**: `Content-Encoding: gzip`
3. **WebSocket**: 升级协议支持
4. **HTTP/2**: 多路复用

### 安全考虑

1. **路径遍历防护**: 检查 `..` 攻击
2. **请求大小限制**: 防止 DoS
3. **超时处理**: 慢速攻击防护

## 小结

本教程实现了一个基础的 HTTP/1.1 服务器，核心要点：

1. **流式解析**: 状态机处理不完整请求
2. **路由系统**: 方法 + 路径匹配
3. **Keep-Alive**: 连接复用
4. **静态文件**: 简单文件服务
5. **传输抽象**: 依赖 `IStreamServer` 接口，不绑定具体传输层

HTTP 服务器通过 `IStreamServer` 抽象接口与传输层交互，可以无修改地运行在 TCP、TLS、QUIC 等不同传输层上。

下一教程我们将实现 HTTP 客户端，让协议栈能够主动发起 HTTP 请求。
