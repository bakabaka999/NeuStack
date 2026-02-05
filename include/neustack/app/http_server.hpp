#ifndef NEUSTACK_APP_HTTP_SERVER_HPP
#define NEUSTACK_APP_HTTP_SERVER_HPP

#include "neustack/app/http_types.hpp"
#include "neustack/app/http_parser.hpp"
#include "neustack/transport/stream.hpp"
#include <unordered_map>

namespace neustack {

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
 *   server.get("/api/users", handle_users);
 *   server.post("/api/login", handle_login);
 *
 *   server.listen(80);
 */

class HttpServer {
public:
    explicit HttpServer(IStreamServer &transport) : _transport(transport) {}

    // 注册路由
    void get(const std::string &path, HttpHandler handler);
    void post(const std::string &path, HttpHandler handler);
    void put(const std::string &path, HttpHandler handler);
    void del(const std::string &path, HttpHandler handler);

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

    HttpHandler _not_found_handler;
    std::string _static_prefix;
    std::string _static_dir;

    // 每个连接的状态
    struct ConnectionState {
        HttpRequestParser parser;
        bool keep_alive = true;
        std::string pending_data;   // 待发送的数据
        size_t pending_offset = 0;  // 已发送的偏移量
    };
    std::unordered_map<IStreamConnection *, ConnectionState> _connections;

    // 处理连接
    void on_receive(IStreamConnection *conn, const uint8_t *data, size_t len);
    void on_close(IStreamConnection *conn);

    // 分发请求
    HttpResponse dispatch(const HttpRequest &request);

    // 发送响应（支持分段发送）
    void send_response(IStreamConnection *conn, const HttpResponse &response);

    // 继续发送待发送数据，返回是否全部发送完成
    bool flush_pending(IStreamConnection *conn);
};

} // namespace neustack


#endif // NEUSTACK_APP_HTTP_SERVER_HPP