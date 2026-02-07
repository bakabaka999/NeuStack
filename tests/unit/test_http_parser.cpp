#include <catch2/catch_test_macros.hpp>
#include "neustack/app/http_parser.hpp"
#include "neustack/app/http_types.hpp"
#include <string>

using namespace neustack;

TEST_CASE("HttpRequestParser functionality", "[app][http]") {
    HttpRequestParser parser;

    SECTION("Complete GET request parsing") {
        std::string raw = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
        size_t consumed = parser.feed(raw);

        CHECK(consumed == raw.size());
        CHECK(parser.is_complete());
        CHECK_FALSE(parser.has_error());

        const auto& req = parser.request();
        CHECK(req.method == HttpMethod::GET);
        CHECK(req.path == "/index.html");
        CHECK(req.version == "HTTP/1.1");
        CHECK(req.get_header("Host") == "example.com");
        CHECK(req.body.empty());
    }

    SECTION("POST request with body") {
        std::string raw = "POST /api/echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
        parser.feed(raw);

        CHECK(parser.is_complete());
        const auto& req = parser.request();
        CHECK(req.method == HttpMethod::POST);
        CHECK(req.body == "hello");
        CHECK(req.body.length() == 5);
    }

    SECTION("Streaming / Fragmented data") {
        parser.feed(std::string("GET /test HT"));
        CHECK_FALSE(parser.is_complete());

        parser.feed(std::string("TP/1.1\r\n\r\n"));
        CHECK(parser.is_complete());
        CHECK(parser.request().path == "/test");
    }

    SECTION("Multiple headers with same key") {
        std::string raw = "GET / HTTP/1.1\r\nSet-Cookie: id=1\r\nSet-Cookie: user=admin\r\n\r\n";
        parser.feed(raw);

        CHECK(parser.is_complete());
        auto cookies = parser.request().get_headers("Set-Cookie");
        CHECK(cookies.size() == 2);
        CHECK(cookies[0] == "id=1");
        CHECK(cookies[1] == "user=admin");
    }

    SECTION("Reset and reuse") {
        parser.feed(std::string("GET /first HTTP/1.1\r\n\r\n"));
        CHECK(parser.is_complete());

        parser.reset();
        CHECK_FALSE(parser.is_complete());

        parser.feed(std::string("GET /second HTTP/1.1\r\n\r\n"));
        CHECK(parser.is_complete());
        CHECK(parser.request().path == "/second");
    }

    SECTION("Malformed request") {
        parser.feed(std::string("INVALID\r\n\r\n"));
        CHECK((parser.has_error() || !parser.is_complete()));
    }
}

TEST_CASE("HttpResponseParser and Serialization", "[app][http]") {
    HttpResponseParser parser;

    SECTION("Response serialization round-trip") {
        HttpResponse res;
        res.content_type("text/html").set_body("hello");

        std::string serialized = res.serialize();
        parser.feed(serialized);
        CHECK(parser.is_complete());

        const auto& parsed = parser.response();
        CHECK(parsed.status == HttpStatus::OK);
        CHECK(parsed.body == "hello");
        auto it = parsed.headers.find("Content-Type");
        CHECK(it != parsed.headers.end());
        CHECK(it->second[0] == "text/html");
    }

    SECTION("Parse 404 Not Found") {
        std::string raw = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        parser.feed(raw);

        CHECK(parser.is_complete());
        CHECK(parser.response().status == HttpStatus::NotFound);
    }
}
