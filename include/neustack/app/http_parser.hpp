#ifndef NEUSTACK_APP_HTTP_PARSER_HPP
#define NEUSTACK_APP_HTTP_PARSER_HPP

#include "neustack/app/http_types.hpp"
#include <optional>

namespace neustack {

/**
 * HTTP 报文解析器基类
 *
 * 特点：
 * - 流式解析：数据可以分多次喂入
 * - 虚类继承：请求与响应解析器继承此类
 */
class HttpParser {
public:
    enum class State {
        FirstLine,  // 解析首行（请求行/状态行）
        Headers,    // 解析头部
        Body,       // 解析请求体
        Complete,   // 解析完成
        Error       // 解析错误
    };

    virtual ~HttpParser() = default;

    // 喂入数据
    virtual size_t feed(const uint8_t *data, size_t len) = 0;
    size_t feed(const std::string &data) {
        return feed(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }

    // 状态查询
    State state() const { return _state; }
    bool is_complete() const { return _state == State::Complete; }
    bool has_error() const { return _state == State::Error; }

    // 重置
    virtual void reset() = 0;

    // 错误信息
    const std::string &error() const { return _error; }

protected:
    State _state = State::FirstLine;
    std::string _buffer;
    std::string _error;
    size_t _content_length = 0;

    // 公共解析方法
    bool parse_headers(std::unordered_map<std::string, std::vector<std::string>> &headers);
    bool parse_body(std::string &body);
};

/**
 * HTTP 请求解析器
 */
class HttpRequestParser : public HttpParser {
public:
    HttpRequestParser() = default;

    // 喂入数据
    size_t feed(const uint8_t *data, size_t len) override;

    // 获取解析结果
    const HttpRequest &request() const { return _request; }
    HttpRequest take_request() { return std::move(_request); }

    // 重置
    void reset() override;

private:
    HttpRequest _request;

    bool parse_request_line();
};

/**
 * HTTP 响应解析器
 */
class HttpResponseParser : public HttpParser {
public:
    HttpResponseParser() = default;

    // 喂入数据
    size_t feed(const uint8_t *data, size_t len) override;

    // 获取解析结果
    const HttpResponse &response() const { return _response; }
    HttpResponse take_response() { return std::move(_response); }

    // 重置
    void reset() override;

private:
    HttpResponse _response;
    bool _chunked = false;

    bool parse_status_line();
};

} // namespace neustack

#endif // NEUSTACK_APP_HTTP_PARSER_HPP
