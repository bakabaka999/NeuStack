/**
 * HTTP Benchmark
 * * 测量 HTTP 请求/响应的解析和序列化性能
 */

#include "neustack/app/http_parser.hpp"
#include "neustack/app/http_types.hpp"
#include "neustack/common/log.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>

using namespace neustack;
using Clock = std::chrono::high_resolution_clock;

// 防止编译器优化
template <typename T>
void do_not_optimize(T const& val) {
    volatile T sink = val;
    (void)sink;
}

static constexpr int ITERATIONS = 500000;

/**
 * 1. HTTP GET 请求解析吞吐量
 */
void bench_request_parse() {
    HttpRequestParser parser;
    std::string request = 
        "GET /api/data?id=123 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: NeuStack/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        parser.reset();
        size_t n = parser.feed(request);
        do_not_optimize(n);
        
        // 简单验证 (在 Release 模式下可能会被优化，所以用 do_not_optimize)
        bool complete = parser.is_complete();
        do_not_optimize(complete);
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double k_rps = (ITERATIONS / diff.count()) / 1000.0; // K req/s

    std::cout << "HTTP GET parse:      " 
              << std::fixed << std::setprecision(2) << k_rps << " K req/s" << std::endl;
}

/**
 * 2. HTTP POST 请求解析吞吐量 (带 1KB Body)
 */
void bench_request_parse_with_body() {
    HttpRequestParser parser;
    
    std::string headers = 
        "POST /api/submit HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 1024\r\n"
        "\r\n";
    
    std::string body(1024, '{'); // 模拟 1KB 数据
    std::string request = headers + body;

    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        parser.reset();
        size_t n = parser.feed(request);
        do_not_optimize(n);
        
        bool complete = parser.is_complete();
        do_not_optimize(complete);
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double k_rps = (ITERATIONS / diff.count()) / 1000.0;

    std::cout << "HTTP POST parse:     " 
              << std::fixed << std::setprecision(2) << k_rps << " K req/s (1KB body)" << std::endl;
}

/**
 * 3. HTTP 响应序列化吞吐量
 */
void bench_response_serialize() {
    HttpResponse resp;
    std::string body(1024, 'X'); // 1KB body
    
    resp.status = HttpStatus::OK;
    resp.content_type("application/json");
    resp.set_header("Server", "NeuStack/1.0");
    resp.set_header("Connection", "keep-alive");
    resp.set_body(body);

    auto start = Clock::now();

    size_t total_bytes = 0;
    for (int i = 0; i < ITERATIONS; ++i) {
        std::string s = resp.serialize();
        total_bytes += s.size();
        do_not_optimize(s);
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double k_rps = (ITERATIONS / diff.count()) / 1000.0;
    double mbs = (total_bytes / diff.count()) / (1024.0 * 1024.0);

    std::cout << "HTTP resp serialize: " 
              << std::fixed << std::setprecision(2) << k_rps << " K resp/s "
              << "(" << mbs << " MB/s)" << std::endl;
}

/**
 * 4. HTTP 响应解析吞吐量
 */
void bench_response_parse() {
    // 先构造一个合法的响应字符串
    HttpResponse resp;
    resp.status = HttpStatus::OK;
    resp.content_type("text/html");
    resp.set_body("<html><body>Hello</body></html>");
    std::string raw_response = resp.serialize();

    HttpResponseParser parser;

    auto start = Clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        parser.reset();
        size_t n = parser.feed(raw_response);
        do_not_optimize(n);
        
        bool complete = parser.is_complete();
        do_not_optimize(complete);
    }

    auto end = Clock::now();

    std::chrono::duration<double> diff = end - start;
    double k_rps = (ITERATIONS / diff.count()) / 1000.0;

    std::cout << "HTTP resp parse:     " 
              << std::fixed << std::setprecision(2) << k_rps << " K req/s" << std::endl;
}

int main() {
    // 禁用日志以避免干扰
    Logger::instance().set_level(LogLevel::ERROR);

    std::cout << "=== NeuStack HTTP Benchmark ===" << std::endl;

    bench_request_parse();
    bench_request_parse_with_body();
    bench_response_serialize();
    bench_response_parse();

    return 0;
}