#include "neustack/app/http_server.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"
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
    {".csv",  "text/csv"},

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
    {".otf",   "font/otf"},
    {".eot",   "application/vnd.ms-fontobject"},

    // 压缩包
    {".zip",  "application/zip"},
    {".gz",   "application/gzip"},
    {".tar",  "application/x-tar"},

    // 其他
    {".pdf",  "application/pdf"},
    {".mp3",  "audio/mpeg"},
    {".mp4",  "video/mp4"},
    {".wasm", "application/wasm"},
};

// 获取文件扩展名
std::string get_extension(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) {
        return "";
    }
    return path.substr(pos);
}

// 根据扩展名获取 MIME 类型
const char* get_mime_type(const std::string& path) {
    std::string ext = get_extension(path);
    auto it = MIME_TYPES.find(ext);
    if (it != MIME_TYPES.end()) {
        return it->second.c_str();
    }
    return "application/octet-stream";
}

} // anonymous namespace


using namespace neustack;

void HttpServer::get(const std::string &path, HttpHandler handler) {
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

void HttpServer::get_chunked(const std::string &path, ChunkedHandler handler) {
    _chunked_routes[path] = std::move(handler);
}

void HttpServer::set_not_found_handler(HttpHandler handler) {
    _not_found_handler = std::move(handler);
}

void HttpServer::serve_static(const std::string &url_prefix, const std::string &dir_path) {
    _static_prefix = url_prefix;
    _static_dir = dir_path;
}

int HttpServer::listen(uint16_t port) {
    // 使用 IStreamServer 接口监听
    return _transport.listen(port, [this](IStreamConnection *conn) -> StreamCallbacks {
        // 新连接：初始化状态
        _connections[conn] = ConnectionState{};
        LOG_DEBUG(HTTP, "new connection from %s:%u",
                  ip_to_string(conn->remote_ip()).c_str(), conn->remote_port());
        
        return StreamCallbacks{ // 给运输层对应的回调
            .on_receive = [this](IStreamConnection* conn, const uint8_t* data, size_t len) {
                on_receive(conn, data, len);
            },
            .on_close = [this](IStreamConnection* conn) {
                on_close(conn);
            }
        };
    });
}

void HttpServer::on_receive(IStreamConnection *conn, const uint8_t *data, size_t len) {
    auto it = _connections.find(conn);
    if (it == _connections.end()) return; // 没找到

    auto &state = it->second;

    // 如果正在进行流式发送，继续发送
    if (state.chunked_generator) {
        send_next_chunk(conn);
        return;
    }

    // 如果还有待发送数据，先尝试发送
    if (!state.pending_data.empty()) {
        flush_pending(conn);
        // 如果还没发完，暂不处理新请求
        if (!state.pending_data.empty()) {
            return;
        }
    }

    state.parser.feed(data, len);

    // 请求完整时再处理
    while (state.parser.is_complete()) {
        HttpRequest request = state.parser.take_request();

        LOG_INFO(HTTP, "%s %s",
                 http_method_name(request.method),
                 request.path.c_str());

        // 检查 Keep-Alive
        std::string conn_header = request.get_header("Connection");
        state.keep_alive = (conn_header != "close");

        // 尝试流式路由
        if (request.method == HttpMethod::GET && dispatch_chunked(conn, request)) {
            // 流式响应已开始，重置解析器并返回
            state.parser.reset();
            return;
        }

        // 分发并发送响应
        HttpResponse response = dispatch(request);

        // 设置 Connection 头
        if (!state.keep_alive) {
            response.set_header("Connection", "close");
        }

        send_response(conn, response);

        // 重置解析器以处理下一个请求
        state.parser.reset();

        // 如果响应还没发完，暂停处理后续请求
        if (!state.pending_data.empty()) {
            break;
        }

        // 非 Keep-Alive，关闭连接
        if (!state.keep_alive) {
            conn->close();
            break;
        }
    }

    if (state.parser.has_error()) {
        LOG_WARN(HTTP, "parse error: %s", state.parser.error().c_str());

        HttpResponse response;
        response.status = HttpStatus::BadRequest;
        response.set_body("Bad Request");
        send_response(conn, response);

        conn->close();
    }
}

void HttpServer::on_close(IStreamConnection *conn) {
    _connections.erase(conn);
    conn->close();  // 对方关闭后，我们也关闭，避免 TCB 卡在 CLOSE_WAIT
    LOG_DEBUG(HTTP, "connection closed");
}

HttpResponse HttpServer::dispatch(const HttpRequest &request) {
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

        std::string relative_path = request.path.substr(_static_prefix.length());

        // 安全检查：防止路径穿越攻击
        // 拒绝包含 ".." 或以 "/" 开头的相对路径
        if (relative_path.find("..") != std::string::npos ||
            (!relative_path.empty() && relative_path[0] == '/')) {
            HttpResponse response;
            response.status = HttpStatus::Forbidden;
            response.content_type("text/html");
            response.set_body("<html><body><h1>403 Forbidden</h1></body></html>");
            return response;
        }

        std::string file_path = _static_dir + "/" + relative_path;

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

void HttpServer::send_response(IStreamConnection *conn, const HttpResponse &response) {
    auto it = _connections.find(conn);
    if (it == _connections.end()) return;

    auto &state = it->second;

    // 序列化响应
    state.pending_data = response.serialize();
    state.pending_offset = 0;

    // 尝试发送
    flush_pending(conn);
}

bool HttpServer::flush_pending(IStreamConnection *conn) {
    auto it = _connections.find(conn);
    if (it == _connections.end()) return true;

    auto &state = it->second;

    while (state.pending_offset < state.pending_data.size()) {
        const uint8_t *data = reinterpret_cast<const uint8_t*>(
            state.pending_data.data() + state.pending_offset);
        size_t remaining = state.pending_data.size() - state.pending_offset;

        ssize_t sent = conn->send(data, remaining);

        if (sent <= 0) {
            // 缓冲区满或出错，稍后重试
            LOG_DEBUG(HTTP, "send buffer full, %zu bytes pending", remaining);
            return false;
        }

        state.pending_offset += sent;
        LOG_DEBUG(HTTP, "sent %zd bytes, %zu remaining",
                  sent, state.pending_data.size() - state.pending_offset);
    }

    // 全部发送完成，清理
    state.pending_data.clear();
    state.pending_offset = 0;

    // 如果是非 Keep-Alive，现在可以关闭了
    if (!state.keep_alive) {
        conn->close();
    }

    return true;
}

void HttpServer::poll() {
    // 遍历所有连接，尝试 flush 待发送数据或继续流式发送
    for (auto &[conn, state] : _connections) {
        if (state.chunked_generator) {
            send_next_chunk(conn);
        } else if (!state.pending_data.empty()) {
            flush_pending(conn);
        }
    }
}

bool HttpServer::dispatch_chunked(IStreamConnection *conn, const HttpRequest &request) {
    auto it = _chunked_routes.find(request.path);
    if (it == _chunked_routes.end()) {
        return false;
    }

    auto state_it = _connections.find(conn);
    if (state_it == _connections.end()) {
        return false;
    }

    auto &state = state_it->second;

    // 创建生成器
    state.chunked_generator = it->second(request);
    if (!state.chunked_generator) {
        return false;
    }

    state.chunked_header_sent = false;
    state.chunked_transfer_encoding = false;

    // 开始发送
    send_next_chunk(conn);
    return true;
}

bool HttpServer::send_next_chunk(IStreamConnection *conn) {
    auto it = _connections.find(conn);
    if (it == _connections.end()) return true;

    auto &state = it->second;

    if (!state.chunked_generator) {
        return true;  // 没有流式响应
    }

    // 先发送 HTTP 头（只发送一次）
    if (!state.chunked_header_sent) {
        size_t total = state.chunked_generator->total_size();
        std::string content_type = state.chunked_generator->content_type();
        state.chunked_transfer_encoding = (total == 0);

        std::string header = "HTTP/1.1 200 OK\r\n";
        header += "Content-Type: " + content_type + "\r\n";
        header += "Cache-Control: no-cache\r\n";

        if (!state.chunked_transfer_encoding) {
            // 已知大小，使用 Content-Length
            header += "Content-Length: " + std::to_string(total) + "\r\n";
        } else {
            // 未知大小，使用 chunked transfer encoding
            header += "Transfer-Encoding: chunked\r\n";
        }

        if (!state.keep_alive) {
            header += "Connection: close\r\n";
        }
        header += "\r\n";

        state.pending_data = std::move(header);
        state.pending_offset = 0;
        state.chunked_header_sent = true;
    }

    // 先发送待发送的数据（头或上一个 chunk）
    if (!state.pending_data.empty()) {
        while (state.pending_offset < state.pending_data.size()) {
            const uint8_t *data = reinterpret_cast<const uint8_t*>(
                state.pending_data.data() + state.pending_offset);
            size_t remaining = state.pending_data.size() - state.pending_offset;

            ssize_t sent = conn->send(data, remaining);
            if (sent <= 0) {
                // 缓冲区满，稍后重试
                return false;
            }
            state.pending_offset += sent;
        }

        // 清理已发送的数据
        state.pending_data.clear();
        state.pending_offset = 0;
    }

    // 生成并发送下一块数据
    while (state.chunked_generator->has_more()) {
        // 生成一块数据（32KB）
        std::string chunk = state.chunked_generator->next_chunk(32768);
        if (chunk.empty()) {
            break;
        }

        if (state.chunked_transfer_encoding) {
            std::ostringstream framed;
            framed << std::hex << chunk.size() << "\r\n";
            framed << chunk << "\r\n";
            chunk = framed.str();
        }

        const uint8_t *data = reinterpret_cast<const uint8_t*>(chunk.data());
        size_t len = chunk.size();
        size_t offset = 0;

        while (offset < len) {
            ssize_t sent = conn->send(data + offset, len - offset);
            if (sent <= 0) {
                // 缓冲区满，保存剩余数据到 pending
                state.pending_data = chunk.substr(offset);
                state.pending_offset = 0;
                return false;
            }
            offset += sent;
        }
    }

    // 检查是否完成
    if (!state.chunked_generator->has_more()) {
        if (state.chunked_transfer_encoding) {
            state.pending_data = "0\r\n\r\n";
            state.pending_offset = 0;

            while (state.pending_offset < state.pending_data.size()) {
                const uint8_t *data = reinterpret_cast<const uint8_t*>(
                    state.pending_data.data() + state.pending_offset);
                size_t remaining = state.pending_data.size() - state.pending_offset;

                ssize_t sent = conn->send(data, remaining);
                if (sent <= 0) {
                    return false;
                }
                state.pending_offset += sent;
            }

            state.pending_data.clear();
            state.pending_offset = 0;
        }

        LOG_DEBUG(HTTP, "chunked response complete");
        state.chunked_generator.reset();
        state.chunked_transfer_encoding = false;

        // 非 Keep-Alive，关闭连接
        if (!state.keep_alive) {
            conn->close();
        }
        return true;
    }

    return false;
}
