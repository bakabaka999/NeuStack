#ifndef NEUSTACK_COMMON_JSON_BUILDER_HPP
#define NEUSTACK_COMMON_JSON_BUILDER_HPP

#include <string>
#include <string_view>
#include <cstdint>

namespace neustack {

/**
 * 轻量 JSON 构建器
 *
 * 零外部依赖，手动拼接字符串，支持 pretty print。
 * 使用方式：
 *
 *   JsonBuilder b(true);   // pretty = true
 *   b.begin_object();
 *     b.key("name"); b.write_string("neustack"); b.comma();
 *     b.key("count"); b.write_uint64(42);
 *   b.end_object();
 *   std::string json = std::move(b.buf);
 *
 * 注意：调用者负责正确放置 comma()——除最后一个字段外每个字段后都需要调用。
 */
class JsonBuilder {
public:
    std::string buf;

    explicit JsonBuilder(bool pretty_print, size_t reserve = 4096)
        : _pretty(pretty_print), _depth(0)
    {
        buf.reserve(reserve);
    }

    // ─── 结构 ───

    void begin_object() {
        buf.push_back('{');
        _newline();
        ++_depth;
    }

    void end_object() {
        --_depth;
        _newline();
        _indent();
        buf.push_back('}');
    }

    void begin_array() {
        buf.push_back('[');
        _newline();
        ++_depth;
    }

    void end_array() {
        --_depth;
        _newline();
        _indent();
        buf.push_back(']');
    }

    // ─── 键值 ───

    void key(std::string_view k) {
        _indent();
        _write_quoted(k);
        buf.push_back(':');
        if (_pretty) buf.push_back(' ');
    }

    void comma() {
        buf.push_back(',');
        _newline();
    }

    // ─── 值 ───

    void write_string(std::string_view s) {
        _write_quoted(s);
    }

    void write_uint64(uint64_t v) {
        format_uint64(buf, v);
    }

    void write_double(double v) {
        format_double(buf, v);
    }

    void write_bool(bool v) {
        buf.append(v ? "true" : "false");
    }

    void write_null() {
        buf.append("null");
    }

    void write_raw(std::string_view s) {
        buf.append(s);
    }

    // ─── 静态工具函数（可单独使用）───

    static void escape_string(std::string& out, std::string_view in);
    static void format_uint64(std::string& out, uint64_t v);
    static void format_double(std::string& out, double v);

private:
    bool _pretty;
    int  _depth;

    void _indent() {
        if (!_pretty) return;
        for (int i = 0; i < _depth; ++i)
            buf.append("  ");
    }

    void _newline() {
        if (_pretty) buf.push_back('\n');
    }

    void _write_quoted(std::string_view s) {
        buf.push_back('"');
        escape_string(buf, s);
        buf.push_back('"');
    }
};

} // namespace neustack

#endif // NEUSTACK_COMMON_JSON_BUILDER_HPP
