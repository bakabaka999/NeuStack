#ifndef NEUSTACK_COMMON_STRING_UTILS_HPP
#define NEUSTACK_COMMON_STRING_UTILS_HPP

#include <string>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace neustack {

/**
 * 大小写不敏感的字符串比较
 * 用于 HTTP header name 等场景（RFC 7230）
 */
inline bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/**
 * 在 headers map 中查找 key（大小写不敏感）
 */
template <typename Map>
auto find_header_ignore_case(Map &headers, const std::string &key)
    -> decltype(headers.begin()) {
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (iequals(it->first, key)) {
            return it;
        }
    }
    return headers.end();
}

/**
 * 安全的字符串转整数，失败时返回默认值
 */
inline int stoi_safe(const std::string &str, int default_val = 0) {
    if (str.empty()) return default_val;
    try {
        return std::stoi(str);
    } catch (...) {
        return default_val;
    }
}

/**
 * 安全的字符串转无符号长整数，失败时返回默认值
 */
inline unsigned long stoul_safe(const std::string &str, unsigned long default_val = 0) {
    if (str.empty()) return default_val;
    try {
        return std::stoul(str);
    } catch (...) {
        return default_val;
    }
}

} // namespace neustack

#endif // NEUSTACK_COMMON_STRING_UTILS_HPP
