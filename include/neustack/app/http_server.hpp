#ifndef NEUSTACK_APP_HTTP_SERVER_HPP
#define NEUSTACK_APP_HTTP_SERVER_HPP

#include "neustack/app/http_types.hpp"
#include "neustack/app/http_parser.hpp"
#include "neustack/transport/stream.hpp"
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstring>

namespace neustack {

/**
 * 流式响应生成器基类
 *
 * 用于大文件下载等场景，避免一次性加载整个响应到内存
 */
class IChunkedResponse {
public:
    virtual ~IChunkedResponse() = default;

    // 获取下一块数据，返回空表示结束
    // 注意：chunk_size 是建议大小，实际返回可以更小
    virtual std::string next_chunk(size_t chunk_size = 32768) = 0;

    // 是否还有更多数据
    virtual bool has_more() const = 0;

    // 获取总大小（如果已知），用于 Content-Length
    // 返回 0 表示未知，将使用 chunked transfer encoding
    virtual size_t total_size() const { return 0; }

    // 获取 Content-Type
    virtual std::string content_type() const { return "application/octet-stream"; }
};

/**
 * 随机数据生成器 - 用于下载测试
 */
class RandomDataGenerator : public IChunkedResponse {
public:
    explicit RandomDataGenerator(size_t total_bytes)
        : _total_bytes(total_bytes), _sent_bytes(0), _state(0xDEADBEEF) {}

    std::string next_chunk(size_t chunk_size = 32768) override {
        if (_sent_bytes >= _total_bytes) return "";

        size_t to_generate = std::min(chunk_size, _total_bytes - _sent_bytes);
        std::string chunk(to_generate, '\0');

        for (size_t i = 0; i < to_generate; i += 4) {
            _state ^= _state << 13;
            _state ^= _state >> 17;
            _state ^= _state << 5;
            size_t remaining = std::min(to_generate - i, static_cast<size_t>(4));
            std::memcpy(&chunk[i], &_state, remaining);
        }

        _sent_bytes += to_generate;
        return chunk;
    }

    bool has_more() const override { return _sent_bytes < _total_bytes; }
    size_t total_size() const override { return _total_bytes; }

private:
    size_t _total_bytes;
    size_t _sent_bytes;
    uint32_t _state;
};

// 流式响应处理器
using ChunkedHandler = std::function<std::unique_ptr<IChunkedResponse>(const HttpRequest &)>;

/**
 * HTTP 服务器
 *
 * 使用示例:
 *   HttpServer server(tcp_layer);
 *
 *   server.get("/", [](const HttpRequest& req) {
 *       return HttpResponse().set_body("Hello, World!");
 *   });
 *
 *   // 流式大文件下载
 *   server.get_chunked("/download/100m", [](const HttpRequest& req) {
 *       return std::make_unique<RandomDataGenerator>(100 * 1024 * 1024);
 *   });
 *
 *   server.listen(80);
 */

class HttpServer {
public:
    explicit HttpServer(IStreamServer &transport) : _transport(transport) {}

    // 注册路由（普通响应）
    void get(const std::string &path, HttpHandler handler);
    void post(const std::string &path, HttpHandler handler);
    void put(const std::string &path, HttpHandler handler);
    void del(const std::string &path, HttpHandler handler);

    // 注册流式路由（大文件下载）
    void get_chunked(const std::string &path, ChunkedHandler handler);

    // 注册静态文件目录
    void serve_static(const std::string &url_prefix, const std::string &dir_path);

    // 设置404处理器
    void set_not_found_handler(HttpHandler handler);

    // 启动监听
    int listen(uint16_t port);

    // 定时调用，用于 flush 待发送数据
    void poll();

private:
    IStreamServer &_transport;

    // 路由表: method -> path -> handler
    std::unordered_map<HttpMethod, std::unordered_map<std::string, HttpHandler>> _routes;

    // 流式路由表
    std::unordered_map<std::string, ChunkedHandler> _chunked_routes;

    HttpHandler _not_found_handler;
    std::string _static_prefix;
    std::string _static_dir;

    // 每个连接的状态
    struct ConnectionState {
        HttpRequestParser parser;
        bool keep_alive = true;

        // 普通响应的待发送数据
        std::string pending_data;
        size_t pending_offset = 0;

        // 流式响应
        std::unique_ptr<IChunkedResponse> chunked_generator;
        bool chunked_header_sent = false;
        bool chunked_transfer_encoding = false;
    };
    std::unordered_map<IStreamConnection *, ConnectionState> _connections;

    // 处理连接
    void on_receive(IStreamConnection *conn, const uint8_t *data, size_t len);
    void on_close(IStreamConnection *conn);

    // 分发请求
    HttpResponse dispatch(const HttpRequest &request);

    // 尝试分发到流式处理器，成功返回 true
    bool dispatch_chunked(IStreamConnection *conn, const HttpRequest &request);

    // 发送响应（支持分段发送）
    void send_response(IStreamConnection *conn, const HttpResponse &response);

    // 继续发送待发送数据，返回是否全部发送完成
    bool flush_pending(IStreamConnection *conn);

    // 发送流式响应的下一块
    bool send_next_chunk(IStreamConnection *conn);
};

} // namespace neustack


#endif // NEUSTACK_APP_HTTP_SERVER_HPP
