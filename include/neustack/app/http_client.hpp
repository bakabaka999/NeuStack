#ifndef NEUSTACK_APP_HTTP_CLIENT_HPP
#define NEUSTACK_APP_HTTP_CLIENT_HPP

#include "neustack/app/http_types.hpp"
#include "neustack/app/http_parser.hpp"
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
 * 使用示例：
 *   HttpClient client(tcp_layer);
 *   client.get(server_ip, 80, "/api/data", [](const HttpResponse& resp) {
 *       if (resp.status == HttpStatus::OK) {
 *           printf("Response: %s\n", resp.body.c_str());
 *       }
 *   });   
 */
class HttpClient {
public:
    // 响应回调
    using ResponseCallback = std::function<void(const HttpResponse &response, int error)>;

    // 依赖 IStreamClient 接口
    explicit HttpClient(IStreamClient &transport) : _transport(transport) {}

    /**
     * @brief 发起 GET 请求
     * @param server_ip 服务器 IP
     * @param port 端口
     * @param path 路径
     * @param on_response 响应回调 (error: 0=成功, -1=连接失败, -2=解析失败)
     */
    void get(uint32_t server_ip, uint16_t port, const std::string &path,
             ResponseCallback on_response);

    /**
     * @brief 发起 POST 请求
     */
    void post(uint32_t server_ip, uint16_t port, const std::string &path,
              const std::string &body, const std::string &content_type,
              ResponseCallback on_response);

    /**
     * @brief 发起自定义请求
     */
    void request(uint32_t server_ip, uint16_t port,
                 const HttpRequest &req, ResponseCallback on_response);

    /**
     * @brief 设置默认 Host 头
     */
    void set_default_host(const std::string &host) { _default_host = host; }

    /**
     * @brief 设置默认 User-Agent
     */
    void set_user_agent(const std::string &ua) { _user_agent = ua; }

private:
    IStreamClient &_transport;
    std::string _default_host;
    std::string _user_agent = "NeuStack/1.4";

    // 请求上下文
    struct RequestContext {
        HttpResponseParser parser;
        ResponseCallback callback;
        IStreamConnection *conn = nullptr;
    };
};

} // namespace neustack


#endif
