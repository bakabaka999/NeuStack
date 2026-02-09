#include <catch2/catch_test_macros.hpp>
#include "neustack/app/http_parser.hpp"
#include <cstring>
#include <string>
#include <vector>

using namespace neustack;

TEST_CASE("HTTP Edge: Buffer overflow protection", "[app][http][edge]") {
    HttpRequestParser parser;
    
    // 假设 MAX_BUFFER_SIZE 为 1MB
    const size_t ONE_MB = 1 * 1024 * 1024;
    
    SECTION("超过 1MB 的 header 数据") {
        std::string huge_header = "GET / HTTP/1.1\r\nLong-Header: ";
        huge_header.append(ONE_MB + 10, 'A'); // 超过 1MB
        huge_header.append("\r\n\r\n");
        
        parser.feed(reinterpret_cast<const uint8_t*>(huge_header.data()), huge_header.size());
        
        CHECK(parser.has_error());
        CHECK(parser.error() == "Request too large");
    }

    SECTION("分多次 feed 累计超过 1MB") {
        std::string chunk(1024, 'A');
        std::string start = "GET / HTTP/1.1\r\nX-H: ";
        parser.feed(reinterpret_cast<const uint8_t*>(start.data()), start.size());
        
        // Feed 1024 次 1KB 数据 -> 1MB
        for(int i=0; i<1025; ++i) {
            parser.feed(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
            if (parser.has_error()) break;
        }
        
        CHECK(parser.has_error());
        CHECK(parser.error() == "Request too large");
    }

    SECTION("响应解析器同样有 1MB 限制") {
        HttpResponseParser resp_parser;
        std::string huge_resp = "HTTP/1.1 200 OK\r\nLong-Header: ";
        huge_resp.append(ONE_MB + 10, 'B');
        
        resp_parser.feed(reinterpret_cast<const uint8_t*>(huge_resp.data()), huge_resp.size());
        CHECK(resp_parser.has_error());
    }
}

TEST_CASE("HTTP Edge: Malformed request lines", "[app][http][edge]") {
    HttpRequestParser parser;

    SECTION("空请求行 (只有 CRLF)") {
        std::string data = "\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
    }

    SECTION("只有 method 没有 path: 'GET\\r\\n'") {
        std::string data = "GET\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
        CHECK(parser.error() == "Invalid request line");
    }

    SECTION("没有 HTTP 版本: 'GET /path\\r\\n'") {
        std::string data = "GET /path\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
    }

    SECTION("超长 URI (8KB 的 path)") {
        std::string long_path(8192, 'a');
        std::string data = "GET /" + long_path + " HTTP/1.1\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        CHECK_FALSE(parser.has_error());
        CHECK(parser.is_complete());
        CHECK(parser.request().path == "/" + long_path);
    }
}

TEST_CASE("HTTP Edge: Header parsing edge cases", "[app][http][edge]") {
    HttpRequestParser parser;

    SECTION("header 没有冒号: 'BadHeader\\r\\n'") {
        std::string data = "GET / HTTP/1.1\r\nBadHeader\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
        CHECK(parser.error() == "Invalid header line");
    }

    SECTION("header value 中包含冒号") {
        std::string data = "GET / HTTP/1.1\r\nHost: http://example.com:8080\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        CHECK(parser.is_complete());
        // Headers map 通常是 unordered_map<string, vector<string>>
        // 假设有 get_header_value 辅助或者直接访问
        auto& headers = parser.request().headers;
        REQUIRE(headers.count("Host"));
        CHECK(headers.at("Host")[0] == "http://example.com:8080"); // 实现会去掉前导空格
    }

    SECTION("空 header value: 'Key:\\r\\n'") {
        std::string data = "GET / HTTP/1.1\r\nKey:\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        CHECK(parser.is_complete());
        CHECK(parser.request().headers.count("Key"));
        CHECK(parser.request().headers.at("Key")[0].empty()); // 或包含空格
    }

    SECTION("重复同名 header") {
        std::string data = "GET / HTTP/1.1\r\nKey: 1\r\nKey: 2\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        CHECK(parser.is_complete());
        CHECK(parser.request().headers.at("Key").size() == 2);
    }
}

TEST_CASE("HTTP Edge: Content-Length boundary", "[app][http][edge]") {
    HttpRequestParser parser;

    SECTION("Content-Length: 0 带空 body") {
        std::string data = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.is_complete());
        CHECK(parser.request().body.empty());
    }

    SECTION("Content-Length: 5 但只发了 3 字节 body") {
        std::string data = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n123";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK_FALSE(parser.is_complete());
    }

    SECTION("Content-Length: 5 发了 10 字节") {
        std::string data = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n1234567890";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        // 行为依赖实现：可能截断，也可能视为 pipeline 的下一个请求
        // 这里测试最基本的截断行为
        CHECK(parser.is_complete());
        CHECK(parser.request().body == "12345");
    }

    SECTION("Content-Length 非数字: 'abc'") {
        std::string data = "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        
        // 通常 stoul 失败返回 0 或者解析器报错
        // 假设行为是忽略或视为 0
        CHECK(parser.is_complete());
        CHECK(parser.request().body.empty());
    }
}

TEST_CASE("HTTP Edge: Response status code boundary", "[app][http][edge]") {
    HttpResponseParser parser;

    SECTION("状态码 100 (最小合法)") {
        std::string data = "HTTP/1.1 100 Continue\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.is_complete());
    }

    SECTION("状态码 599 (最大合法)") {
        std::string data = "HTTP/1.1 599 Network Connect Timeout Error\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.is_complete());
    }

    SECTION("状态码 99 (太小)") {
        std::string data = "HTTP/1.1 99 Error\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
    }

    SECTION("状态码 600 (太大)") {
        std::string data = "HTTP/1.1 600 Error\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
    }

    SECTION("状态码非数字") {
        std::string data = "HTTP/1.1 abc OK\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
    }
}

TEST_CASE("HTTP Edge: Chunked transfer encoding", "[app][http][edge]") {
    HttpResponseParser parser;
    SECTION("响应带 Transfer-Encoding: chunked 且有 Content-Length 触发 body 状态") {
        // chunked 检测发生在 headers 解析完、进入 Body 状态后
        // 需要 Content-Length 让状态进入 Body，才会触发 chunked 错误
        std::string data = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 10\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.has_error());
        CHECK(parser.error() == "Chunked transfer not implemented");
    }

    SECTION("无 Content-Length 的 chunked 响应直接完成") {
        // 没有 Content-Length 时，parse_headers 进入 Complete，不触发 chunked 检测
        std::string data = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK(parser.is_complete());
        CHECK_FALSE(parser.has_error());
    }
}

TEST_CASE("HTTP Edge: Incomplete data streaming", "[app][http][edge]") {
    HttpRequestParser parser;

    SECTION("逐字节 feed 一个完整请求") {
        std::string data = "GET / HTTP/1.1\r\n\r\n";
        for (char c : data) {
            CHECK_FALSE(parser.is_complete());
            uint8_t byte = static_cast<uint8_t>(c);
            parser.feed(&byte, 1);
        }
        CHECK(parser.is_complete());
    }

    SECTION("只发 request line 没发 headers 结束的空行") {
        std::string data = "GET / HTTP/1.1\r\nHost: loc";
        parser.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        CHECK_FALSE(parser.is_complete());
        CHECK_FALSE(parser.has_error());
    }
}