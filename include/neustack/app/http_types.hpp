#ifndef NEUSTACK_APP_HTTP_TYPES_HPP
#define NEUSTACK_APP_HTTP_TYPES_HPP

#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

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

} // namespace neustack

// HttpMethod 的 hash 特化（用于 unordered_map key）
namespace std {
template <>
struct hash<neustack::HttpMethod> {
    size_t operator()(neustack::HttpMethod m) const noexcept {
        return static_cast<size_t>(m);
    }
};
} // namespace std

namespace neustack {

// HTTP 状态码
enum class HttpStatus {
    OK = 200,
    Created = 201,
    NoContent = 204,
    MovePermanently = 301,
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
struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    // 获取 header 的第一个值（大多数 header 只有一个值）
    std::string get_header(const std::string &key) const {
        auto it = headers.find(key);
        return (it != headers.end() && !it->second.empty()) ? it->second[0] : "";
    }

    // 获取 header 的所有值（用于 Cookie、Set-Cookie 等多值 header）
    std::vector<std::string> get_headers(const std::string &key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : std::vector<std::string>{};
    }

    bool has_header(const std::string &key) const {
        return headers.count(key) > 0;
    }

    // 添加 header（支持同名多值）
    void add_header(const std::string &key, const std::string &value) {
        headers[key].push_back(value);
    }

    // 报文序列化为字节流
    std::string serialize() const;
};

// HTTP 响应
struct HttpResponse {
    HttpStatus status = HttpStatus::OK;
    std::unordered_map<std::string, std::vector<std::string>> headers;
    std::string body;

    // 设置响应头（覆盖该 key 的所有值）
    HttpResponse &set_header(const std::string &key, const std::string &value) {
        headers[key] = {value};
        return *this;
    }

    // 添加响应头（同名追加，用于 Set-Cookie 等）
    HttpResponse &add_header(const std::string &key, const std::string &value) {
        headers[key].push_back(value);
        return *this;
    }

    // 常用快捷方法
    HttpResponse &content_type(const std::string &type) {
        return set_header("Content-Type", type);
    }

    HttpResponse &set_body(const std::string &content) {
        body = content;
        return set_header("Content-Length", std::to_string(content.size()));
    }

    // 序列化为 HTTP 响应文本
    std::string serialize() const;
};

// 请求处理器
using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

// 辅助函数
const char *http_method_name(HttpMethod method);
HttpMethod parse_http_method(const std::string &method);
const char *http_status_text(HttpStatus status);

} // namespace neustack

#endif // NEUSTACK_APP_HTTP_TYPES_HPP