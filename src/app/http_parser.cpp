#include "neustack/app/http_parser.hpp"
#include "neustack/common/string_utils.hpp"
#include <algorithm>

namespace neustack {

// ============================================================================
// HttpParser 基类 - 公共方法
// ============================================================================

bool HttpParser::parse_headers(std::unordered_map<std::string, std::vector<std::string>> &headers) {
    while (true) {
        auto pos = _buffer.find("\r\n");
        if (pos == std::string::npos) {
            return false;  // 数据不完整
        }

        if (pos == 0) {
            // 空行，头部结束
            _buffer.erase(0, 2);

            // 检查 Content-Length（大小写不敏感）
            auto it = find_header_ignore_case(headers, "Content-Length");
            if (it != headers.end() && !it->second.empty()) {
                _content_length = stoul_safe(it->second[0]);
            }

            // 判断是否有 body
            if (_content_length > 0) {
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

        headers[key].push_back(value);
    }
}

bool HttpParser::parse_body(std::string &body) {
    if (_buffer.size() >= _content_length) {
        body = _buffer.substr(0, _content_length);
        _buffer.erase(0, _content_length);
        _state = State::Complete;
        return true;
    }
    return false;  // 数据不完整
}

// ============================================================================
// HttpRequestParser - 请求解析器
// ============================================================================

size_t HttpRequestParser::feed(const uint8_t *data, size_t len) {
    if (_state == State::Complete || _state == State::Error) {
        return 0;
    }

    _buffer.append(reinterpret_cast<const char *>(data), len);

    while (_state != State::Complete && _state != State::Error) {
        switch (_state) {
            case State::FirstLine:
                if (!parse_request_line()) return len;
                break;
            case State::Headers:
                if (!parse_headers(_request.headers)) return len;
                break;
            case State::Body:
                if (!parse_body(_request.body)) return len;
                break;
            default:
                break;
        }
    }

    return len;
}

bool HttpRequestParser::parse_request_line() {
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

void HttpRequestParser::reset() {
    _state = State::FirstLine;
    _request = HttpRequest{};
    _buffer.clear();
    _error.clear();
    _content_length = 0;
}

// ============================================================================
// HttpResponseParser - 响应解析器
// ============================================================================

size_t HttpResponseParser::feed(const uint8_t *data, size_t len) {
    if (_state == State::Complete || _state == State::Error) {
        return 0;
    }

    _buffer.append(reinterpret_cast<const char *>(data), len);

    while (_state != State::Complete && _state != State::Error) {
        switch (_state) {
            case State::FirstLine:
                if (!parse_status_line()) return len;
                break;
            case State::Headers:
                if (!parse_headers(_response.headers)) return len;
                // 检查 chunked（大小写不敏感）
                {
                    auto it = find_header_ignore_case(_response.headers, "Transfer-Encoding");
                    if (it != _response.headers.end() && !it->second.empty()) {
                        _chunked = (it->second[0].find("chunked") != std::string::npos);
                    }
                }
                if (_chunked && _state == State::Body) {
                    // 暂不支持 chunked
                    _state = State::Error;
                    _error = "Chunked transfer not implemented";
                    return len;
                }
                break;
            case State::Body:
                if (!parse_body(_response.body)) return len;
                break;
            default:
                break;
        }
    }

    return len;
}

bool HttpResponseParser::parse_status_line() {
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

    // 状态码（安全解析）
    std::string status_str;
    if (sp2 != std::string::npos) {
        status_str = line.substr(sp1 + 1, sp2 - sp1 - 1);
    } else {
        status_str = line.substr(sp1 + 1);
    }

    int status_code = stoi_safe(status_str, 0);
    if (status_code < 100 || status_code > 599) {
        _state = State::Error;
        _error = "Invalid status code";
        return true;
    }
    _response.status = static_cast<HttpStatus>(status_code);

    _state = State::Headers;
    return true;
}

void HttpResponseParser::reset() {
    _state = State::FirstLine;
    _response = HttpResponse{};
    _buffer.clear();
    _error.clear();
    _content_length = 0;
    _chunked = false;
}

} // namespace neustack
