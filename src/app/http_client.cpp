#include "neustack/app/http_client.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

using namespace neustack;

void HttpClient::get(uint32_t server_ip, uint16_t port, const std::string &path,
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
    // 创建请求上下文
    auto ctx = std::make_shared<RequestContext>();
    ctx->callback = std::move(on_response);

    // 构造完整请求
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

    LOG_DEBUG(HTTP, "client connecting to %s:%u",
              ip_to_string(server_ip).c_str(), port);

    // 发起连接
    _transport.connect(
        server_ip, port, 
        // on_connect 回调函数
        [ctx, request_data](IStreamConnection *conn, int error) {
            if (error != 0 || conn == nullptr) {
                LOG_WARN(HTTP, "client connection failed");
                if (ctx->callback) {
                    ctx->callback(HttpResponse{}, -1);
                }
                return;
            }

            ctx->conn = conn;
            LOG_DEBUG(HTTP, "client connected, sending request");

            // 发送请求
            conn->send(reinterpret_cast<const uint8_t*>(request_data.data()),
                       request_data.size());
        },
        // on_receive 回调函数
        [ctx](IStreamConnection *conn, const uint8_t *data, size_t len) {
            ctx->parser.feed(data, len);

            if (ctx->parser.is_complete()) {
                LOG_DEBUG(HTTP, "client response complete");
                if (ctx->callback) {
                    ctx->callback(ctx->parser.response(), 0);
                    ctx->callback = nullptr;  // 只调用一次
                }
                conn->close();
            } else if (ctx->parser.has_error()) {
                LOG_WARN(HTTP, "client parse error: %s",
                         ctx->parser.error().c_str());
                if (ctx->callback) {
                    ctx->callback(HttpResponse{}, -2);
                    ctx->callback = nullptr;
                }
                conn->close();
            }
        },
        // on_close 回调函数
        [ctx](IStreamConnection* conn) {
            LOG_DEBUG(HTTP, "client connection closed");

            // 如果连接关闭时还没收到完整响应
            if (ctx->callback) {
                if (ctx->parser.state() != HttpParser::State::FirstLine) {
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

