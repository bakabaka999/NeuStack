#include "neustack/common/json_builder.hpp"
#include <cstdio>
#include <cmath>

namespace neustack {

void JsonBuilder::escape_string(std::string& out, std::string_view in) {
    for (char c : in) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(hex);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
}

void JsonBuilder::format_uint64(std::string& out, uint64_t v) {
    char tmp[24];
    int len = std::snprintf(tmp, sizeof(tmp), "%llu",
                            static_cast<unsigned long long>(v));
    out.append(tmp, static_cast<size_t>(len));
}

void JsonBuilder::format_double(std::string& out, double v) {
    if (std::isnan(v)) { out.append("null"); return; }
    if (std::isinf(v)) { out.append(v > 0 ? "1e308" : "-1e308"); return; }

    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char tmp[24];
        int len = std::snprintf(tmp, sizeof(tmp), "%.0f", v);
        out.append(tmp, static_cast<size_t>(len));
    } else {
        char tmp[32];
        int len = std::snprintf(tmp, sizeof(tmp), "%.6g", v);
        out.append(tmp, static_cast<size_t>(len));
    }
}

} // namespace neustack
