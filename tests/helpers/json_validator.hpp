#ifndef NEUSTACK_TEST_JSON_VALIDATOR_HPP
#define NEUSTACK_TEST_JSON_VALIDATOR_HPP

#include <string>
#include <string_view>
#include <stack>

namespace neustack::test {

/**
 * 最简 JSON 格式验证
 *
 * 不做完整语法解析，只检查：
 * 1. 括号匹配
 * 2. 字符串引号匹配
 * 3. 简单的 key 存在性检查
 */
class JsonValidator {
public:
    static bool is_valid(std::string_view json) {
        std::stack<char> brackets;
        bool in_string = false;
        bool escaped = false;

        for (size_t i = 0; i < json.size(); ++i) {
            char c = json[i];

            if (escaped) {
                escaped = false;
                continue;
            }

            if (c == '\\' && in_string) {
                escaped = true;
                continue;
            }

            if (c == '"') {
                in_string = !in_string;
                continue;
            }

            if (in_string) continue;

            if (c == '{' || c == '[') {
                brackets.push(c);
            } else if (c == '}') {
                if (brackets.empty() || brackets.top() != '{') return false;
                brackets.pop();
            } else if (c == ']') {
                if (brackets.empty() || brackets.top() != '[') return false;
                brackets.pop();
            }
        }

        return brackets.empty() && !in_string;
    }

    // 检查 JSON 中是否包含指定的 key ("key":)
    // 这是一个非常简化的检查，可能会有误报（例如 key 出现在字符串值中）
    // 但对于单元测试中检查特定字段是否存在已经足够
    static bool contains_key(std::string_view json, std::string_view key) {
        std::string search = "\"";
        search += key;
        search += "\"";
        return json.find(search) != std::string_view::npos;
    }
};

} // namespace neustack::test

#endif // NEUSTACK_TEST_JSON_VALIDATOR_HPP
