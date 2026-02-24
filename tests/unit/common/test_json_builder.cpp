#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "neustack/common/json_builder.hpp"
#include "../../helpers/json_validator.hpp"

using neustack::JsonBuilder;
using neustack::test::JsonValidator;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("JsonBuilder: empty object", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.end_object();
    CHECK(b.buf == "{}");
    CHECK(JsonValidator::is_valid(b.buf));
}

TEST_CASE("JsonBuilder: single string field", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.key("name"); b.write_string("neustack");
    b.end_object();
    CHECK(b.buf == R"({"name":"neustack"})");
    CHECK(JsonValidator::is_valid(b.buf));
}

TEST_CASE("JsonBuilder: uint64 and double", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.key("count"); b.write_uint64(12345);       b.comma();
    b.key("rate");  b.write_double(3.14);
    b.end_object();
    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("12345"));
    CHECK_THAT(b.buf, ContainsSubstring("3.14"));
}

TEST_CASE("JsonBuilder: bool values", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.key("enabled"); b.write_bool(true);  b.comma();
    b.key("shadow");  b.write_bool(false);
    b.end_object();
    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("true"));
    CHECK_THAT(b.buf, ContainsSubstring("false"));
}

TEST_CASE("JsonBuilder: nested object", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.key("rtt");
    b.begin_object();
    b.key("p50"); b.write_double(120.0); b.comma();
    b.key("p99"); b.write_double(3200.0);
    b.end_object();
    b.end_object();
    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"rtt\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"p50\""));
    CHECK_THAT(b.buf, ContainsSubstring("\"p99\""));
}

TEST_CASE("JsonBuilder: array", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_array();
    b.begin_object(); b.key("id"); b.write_uint64(1); b.end_object(); b.comma();
    b.begin_object(); b.key("id"); b.write_uint64(2); b.end_object();
    b.end_array();
    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\"id\""));
}

TEST_CASE("JsonBuilder: pretty print has newlines and indent", "[common][json_builder]") {
    JsonBuilder compact(false);
    JsonBuilder pretty(true);

    for (auto* b : {&compact, &pretty}) {
        b->begin_object();
        b->key("a"); b->write_uint64(1); b->comma();
        b->key("b"); b->write_uint64(2);
        b->end_object();
    }

    CHECK(pretty.buf.size() > compact.buf.size());
    CHECK_THAT(pretty.buf, ContainsSubstring("\n"));
    CHECK_THAT(pretty.buf, ContainsSubstring("  "));
    CHECK(JsonValidator::is_valid(pretty.buf));
}

TEST_CASE("JsonBuilder: string escaping", "[common][json_builder]") {
    JsonBuilder b(false);
    b.begin_object();
    b.key("msg"); b.write_string("hello\nworld\t\"tab\"");
    b.end_object();
    CHECK(JsonValidator::is_valid(b.buf));
    CHECK_THAT(b.buf, ContainsSubstring("\\n"));
    CHECK_THAT(b.buf, ContainsSubstring("\\t"));
    CHECK_THAT(b.buf, ContainsSubstring("\\\""));
}

TEST_CASE("JsonBuilder: format_double special values", "[common][json_builder]") {
    std::string out;
    JsonBuilder::format_double(out, std::numeric_limits<double>::quiet_NaN());
    CHECK(out == "null");

    out.clear();
    JsonBuilder::format_double(out, std::numeric_limits<double>::infinity());
    CHECK(out == "1e308");

    out.clear();
    JsonBuilder::format_double(out, -std::numeric_limits<double>::infinity());
    CHECK(out == "-1e308");
}

TEST_CASE("JsonBuilder: integer double printed without decimal", "[common][json_builder]") {
    std::string out;
    JsonBuilder::format_double(out, 42.0);
    CHECK(out == "42");
}

TEST_CASE("JsonBuilder: format_uint64 correctness", "[common][json_builder]") {
    std::string out;
    JsonBuilder::format_uint64(out, 0);
    CHECK(out == "0");

    out.clear();
    JsonBuilder::format_uint64(out, UINT64_MAX);
    CHECK(out == "18446744073709551615");
}
