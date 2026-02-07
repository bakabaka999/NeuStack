#include <catch2/catch_test_macros.hpp>
#include "neustack/app/http_types.hpp"
#include <string>

using namespace neustack;

TEST_CASE("HTTP Method and Status utility functions", "[app][http]") {
    SECTION("Method to string conversion") {
        CHECK(std::string(http_method_name(HttpMethod::GET)) == "GET");
        CHECK(std::string(http_method_name(HttpMethod::POST)) == "POST");
        CHECK(std::string(http_method_name(HttpMethod::PUT)) == "PUT");
        CHECK(std::string(http_method_name(HttpMethod::DELETE)) == "DELETE");
        CHECK(std::string(http_method_name(HttpMethod::HEAD)) == "HEAD");
        CHECK(std::string(http_method_name(HttpMethod::OPTIONS)) == "OPTIONS");
        CHECK(std::string(http_method_name(HttpMethod::UNKNOWN)) == "UNKNOWN");
    }

    SECTION("String to method parsing") {
        CHECK(parse_http_method("GET") == HttpMethod::GET);
        CHECK(parse_http_method("POST") == HttpMethod::POST);
        CHECK(parse_http_method("GARBAGE") == HttpMethod::UNKNOWN);
        CHECK(parse_http_method("") == HttpMethod::UNKNOWN);
    }

    SECTION("Status text conversion") {
        CHECK(std::string(http_status_text(HttpStatus::OK)) == "OK");
        CHECK(std::string(http_status_text(HttpStatus::Created)) == "Created");
        CHECK(std::string(http_status_text(HttpStatus::NotFound)) == "Not Found");
        CHECK(std::string(http_status_text(HttpStatus::InternalServerError)) == "Internal Server Error");
    }
}

TEST_CASE("HttpRequest type operations", "[app][http]") {
    HttpRequest req;

    SECTION("Header manipulation") {
        req.add_header("X-Project", "NeuStack");
        
        CHECK(req.has_header("X-Project") == true);
        CHECK(req.has_header("Content-Type") == false);
        CHECK(req.get_header("X-Project") == "NeuStack");
        CHECK(req.get_header("Non-Existent") == "");
    }

    SECTION("Request serialization") {
        req.method = HttpMethod::GET;
        req.path = "/test";
        req.version = "HTTP/1.1";
        req.add_header("Host", "localhost");
        
        std::string raw = req.serialize();
        CHECK(raw.find("GET /test HTTP/1.1\r\n") == 0);
        CHECK(raw.find("Host: localhost\r\n") != std::string::npos);
        CHECK(raw.find("\r\n\r\n") != std::string::npos);
    }
}

TEST_CASE("HttpResponse type operations and chaining", "[app][http]") {
    SECTION("Method chaining and serialization") {
        HttpResponse res;
        res.status = HttpStatus::OK;
        std::string raw = res.content_type("text/html")
                             .set_body("hi")
                             .serialize();

        CHECK(raw.find("HTTP/1.1 200 OK\r\n") == 0);
        CHECK(raw.find("Content-Type: text/html\r\n") != std::string::npos);
        CHECK(raw.find("Content-Length: 2\r\n") != std::string::npos);
        CHECK(raw.ends_with("\r\n\r\nhi"));
    }

    SECTION("Header replacement vs appending") {
        HttpResponse res;
        
        // Test set_header (replace)
        res.set_header("Server", "OldServer");
        res.set_header("Server", "NeuStack-Server");
        std::string raw1 = res.serialize();
        CHECK(raw1.find("Server: NeuStack-Server\r\n") != std::string::npos);
        CHECK(raw1.find("Server: OldServer\r\n") == std::string::npos);

        // Test add_header (append)
        res.add_header("Set-Cookie", "id=1");
        res.add_header("Set-Cookie", "user=admin");
        std::string raw2 = res.serialize();
        
        // Find first occurrence
        size_t first = raw2.find("Set-Cookie: id=1\r\n");
        size_t second = raw2.find("Set-Cookie: user=admin\r\n");
        CHECK(first != std::string::npos);
        CHECK(second != std::string::npos);
        CHECK(first != second);
    }

    SECTION("Body and Content-Length synchronization") {
        HttpResponse res;
        res.set_body("12345");
        std::string raw = res.serialize();
        CHECK(raw.find("Content-Length: 5\r\n") != std::string::npos);
        
        res.set_body("");
        raw = res.serialize();
        CHECK(raw.find("Content-Length: 0\r\n") != std::string::npos);
    }
}