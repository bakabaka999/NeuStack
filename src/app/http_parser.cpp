#include "neustack/app/http_parser.hpp"
#include "neustack/common/string_utils.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

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

    // 缓冲区上限检查，防止 slowloris / 超大请求耗尽内存
    if (_buffer.size() > MAX_BUFFER_SIZE) {
        _state = State::Error;
        _error = "Request too large";
        return len;
    }

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

// 解决 query 参数解析的辅助函数
static std::unordered_map<std::string, std::string> parse_query_string(const std::string& query) {
    std::unordered_map<std::string, std::string> params;
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        else
            params[pair] = "";
    }
    return params;
}

static void split_path_query(HttpRequest& req, const std::string& uri) {
    auto q = uri.find('?');
    if (q != std::string::npos) {
        req.path      = uri.substr(0, q);
        req.raw_query = uri.substr(q + 1);
        req.query_params = parse_query_string(req.raw_query);
    } else {
        req.path = uri;
    }
}

static std::string trim_http_token(const std::string& value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }

    auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

static bool contains_token_ignore_case(const std::string& value, const std::string& token) {
    std::string lower_value = value;
    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string lower_token = token;
    std::transform(lower_token.begin(), lower_token.end(), lower_token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lower_value.find(lower_token) != std::string::npos;
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
    split_path_query(_request, line.substr(sp1 + 1, sp2 - sp1 - 1));
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

    // 缓冲区上限检查
    if (_buffer.size() > MAX_BUFFER_SIZE) {
        _state = State::Error;
        _error = "Response too large";
        return len;
    }

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
                        _chunked = contains_token_ignore_case(it->second[0], "chunked");
                    }
                }
                if (_chunked) {
                    _content_length = 0;
                    _state = State::Body;
                }
                break;
            case State::Body:
                if (_chunked) {
                    if (!parse_chunked_body()) return len;
                } else if (!parse_body(_response.body)) {
                    return len;
                }
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
    _chunk_state = ChunkState::Size;
    _chunk_bytes_remaining = 0;
}

bool HttpResponseParser::parse_chunked_body() {
    while (_state != State::Complete && _state != State::Error) {
        switch (_chunk_state) {
            case ChunkState::Size: {
                auto pos = _buffer.find("\r\n");
                if (pos == std::string::npos) {
                    return false;
                }

                std::string line = _buffer.substr(0, pos);
                _buffer.erase(0, pos + 2);

                auto semi = line.find(';');
                if (semi != std::string::npos) {
                    line = line.substr(0, semi);
                }

                line = trim_http_token(line);
                if (line.empty()) {
                    _state = State::Error;
                    _error = "Invalid chunk size";
                    return true;
                }

                try {
                    size_t parsed = 0;
                    _chunk_bytes_remaining = std::stoul(line, &parsed, 16);
                    if (parsed != line.size()) {
                        throw std::invalid_argument("trailing data");
                    }
                } catch (...) {
                    _state = State::Error;
                    _error = "Invalid chunk size";
                    return true;
                }

                if (_chunk_bytes_remaining == 0) {
                    _chunk_state = ChunkState::Trailers;
                } else {
                    _chunk_state = ChunkState::Data;
                }
                break;
            }

            case ChunkState::Data:
                if (_buffer.size() < _chunk_bytes_remaining) {
                    return false;
                }

                _response.body.append(_buffer.data(), _chunk_bytes_remaining);
                _buffer.erase(0, _chunk_bytes_remaining);
                _chunk_bytes_remaining = 0;
                _chunk_state = ChunkState::DataCRLF;
                break;

            case ChunkState::DataCRLF:
                if (_buffer.size() < 2) {
                    return false;
                }
                if (_buffer.compare(0, 2, "\r\n") != 0) {
                    _state = State::Error;
                    _error = "Invalid chunk terminator";
                    return true;
                }

                _buffer.erase(0, 2);
                _chunk_state = ChunkState::Size;
                break;

            case ChunkState::Trailers: {
                auto pos = _buffer.find("\r\n");
                if (pos == std::string::npos) {
                    return false;
                }

                if (pos == 0) {
                    _buffer.erase(0, 2);
                    _state = State::Complete;
                    return true;
                }

                std::string line = _buffer.substr(0, pos);
                _buffer.erase(0, pos + 2);

                auto colon = line.find(':');
                if (colon == std::string::npos) {
                    _state = State::Error;
                    _error = "Invalid trailer line";
                    return true;
                }

                std::string key = line.substr(0, colon);
                std::string value = trim_http_token(line.substr(colon + 1));
                _response.headers[key].push_back(value);
                break;
            }
        }
    }

    return true;
}

} // namespace neustack
