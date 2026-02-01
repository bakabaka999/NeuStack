#include "neustack/app/http_types.hpp"

namespace neustack {

std::string HttpRequest::serialize() const {
    std::string result;

    // 请求行：METHOD PATH VERSION
    result += http_method_name(method);
    result += " ";
    result += path;
    result += " ";
    result += version.empty() ? "HTTP/1.1" : version;
    result += "\r\n";

    // 请求头
    for (const auto &[key, values] : headers) {
        for (const auto &value : values) {
            result += key;
            result += ": ";
            result += value;
            result += "\r\n";
        }
    }

    // 空行
    result += "\r\n";

    // 请求体
    result += body;

    return result;
}

std::string HttpResponse::serialize() const {
    std::string result;

    // 状态行
    result += "HTTP/1.1 ";
    result += std::to_string(static_cast<int>(status));
    result += " ";
    result += http_status_text(status);
    result += "\r\n";

    // 响应头（每个值独立一行，支持多值 header 如 Set-Cookie）
    for (const auto &[key, values] : headers) {
        for (const auto &value : values) {
            result += key;
            result += ": ";
            result += value;
            result += "\r\n";
        }
    }

    // 空行 + 响应体
    result += "\r\n";
    result += body;
    return result;
}

const char *http_method_name(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default:                  return "UNKNOWN";
    }
}

HttpMethod parse_http_method(const std::string &method) {
    if (method == "GET")     return HttpMethod::GET;
    if (method == "POST")    return HttpMethod::POST;
    if (method == "PUT")     return HttpMethod::PUT;
    if (method == "DELETE")  return HttpMethod::DELETE;
    if (method == "HEAD")    return HttpMethod::HEAD;
    if (method == "OPTIONS") return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

const char *http_status_text(HttpStatus status) {
    switch (status) {
        case HttpStatus::OK:                  return "OK";
        case HttpStatus::Created:             return "Created";
        case HttpStatus::NoContent:           return "No Content";
        case HttpStatus::MovePermanently:     return "Moved Permanently";
        case HttpStatus::Found:               return "Found";
        case HttpStatus::BadRequest:          return "Bad Request";
        case HttpStatus::Unauthorized:        return "Unauthorized";
        case HttpStatus::Forbidden:           return "Forbidden";
        case HttpStatus::NotFound:            return "Not Found";
        case HttpStatus::MethodNotAllowed:    return "Method Not Allowed";
        case HttpStatus::InternalServerError: return "Internal Server Error";
        case HttpStatus::NotImplemented:      return "Not Implemented";
        case HttpStatus::ServiceUnavailable:  return "Service Unavailable";
        default:                              return "Unknown";
    }
}

} // namespace neustack
