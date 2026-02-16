#ifndef NEUSTACK_COMMON_LOG_HPP
#define NEUSTACK_COMMON_LOG_HPP

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <atomic>

// windows.h 定义了 #define ERROR 0, 必须在定义 LogLevel 之前 undef
#ifdef ERROR
#undef ERROR
#endif

namespace neustack {

// ============================================================================
// 日志级别
// ============================================================================

enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
    OFF   = 6   // 完全关闭
};

// ============================================================================
// 日志模块
// ============================================================================

enum class LogModule : uint8_t {
    HAL  = 0,
    IPv4 = 1,
    ICMP = 2,
    ARP  = 3,
    UDP  = 4,
    TCP  = 5,
    HTTP = 6,
    DNS  = 7,
    APP  = 8,
    AI   = 9,
    FW   = 10,  // Firewall
    MAX_MODULES
};

// ============================================================================
// Logger 类 (单例)
// ============================================================================

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // ─── 级别控制 ───

    void set_level(LogLevel level) {
        _global_level.store(level, std::memory_order_relaxed);
    }

    LogLevel level() const {
        return _global_level.load(std::memory_order_relaxed);
    }

    void set_module_level(LogModule module, LogLevel level) {
        auto idx = static_cast<size_t>(module);
        if (idx < static_cast<size_t>(LogModule::MAX_MODULES)) {
            _module_levels[idx].store(level, std::memory_order_relaxed);
        }
    }

    LogLevel module_level(LogModule module) const {
        auto idx = static_cast<size_t>(module);
        if (idx < static_cast<size_t>(LogModule::MAX_MODULES)) {
            return _module_levels[idx].load(std::memory_order_relaxed);
        }
        return LogLevel::OFF;
    }

    // 快速检查 - 内联，无锁
    bool should_log(LogModule module, LogLevel level) const {
        // 先检查全局级别 (最常见的过滤条件)
        if (level < _global_level.load(std::memory_order_relaxed)) {
            return false;
        }
        // 再检查模块级别
        auto mod_level = _module_levels[static_cast<size_t>(module)].load(std::memory_order_relaxed);
        return level >= mod_level;
    }

    // ─── 输出配置 ───

    void set_timestamp(bool enable) { _show_timestamp = enable; }
    bool timestamp() const { return _show_timestamp; }

    void set_color(bool enable) { _use_color = enable; }
    bool color() const { return _use_color; }

    void set_file(FILE* file) {
        std::lock_guard<std::mutex> lock(_mutex);
        _file = file;
    }

    // ─── 核心日志函数 (仅在 should_log 返回 true 时调用) ───

    void log_impl(LogModule module, LogLevel level, const char* fmt, ...)
        __attribute__((format(printf, 4, 5)))
    {
        std::lock_guard<std::mutex> lock(_mutex);

        // 时间戳
        if (_show_timestamp) {
            auto now = std::chrono::steady_clock::now();
            auto ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _start_time).count());
            std::fprintf(_file, "[%6lld.%03lld] ", ms / 1000, ms % 1000);
        }

        // 级别 + 模块
        if (_use_color) {
            std::fprintf(_file, "%s[%s]%s %s%s%s: ",
                level_color(level), level_name(level), RESET,
                module_color(module), module_name(module), RESET);
        } else {
            std::fprintf(_file, "[%s] %s: ", level_name(level), module_name(module));
        }

        // 消息
        va_list args;
        va_start(args, fmt);
        std::vfprintf(_file, fmt, args);
        va_end(args);

        std::fprintf(_file, "\n");

        // 只对 WARN 及以上级别 flush，减少 I/O
        if (level >= LogLevel::WARN) {
            std::fflush(_file);
        }
    }

    // ─── Hex dump (仅在 should_log 返回 true 时调用) ───

    void hexdump_impl(LogModule module, LogLevel level,
                      const uint8_t* data, size_t len, const char* prefix = "") {
        std::lock_guard<std::mutex> lock(_mutex);

        for (size_t i = 0; i < len; i += 16) {
            std::fprintf(_file, "%s%04zx: ", prefix, i);

            // Hex
            for (size_t j = 0; j < 16; ++j) {
                if (i + j < len) {
                    std::fprintf(_file, "%02x ", data[i + j]);
                } else {
                    std::fprintf(_file, "   ");
                }
                if (j == 7) std::fprintf(_file, " ");
            }

            std::fprintf(_file, " |");

            // ASCII
            for (size_t j = 0; j < 16 && i + j < len; ++j) {
                char c = static_cast<char>(data[i + j]);
                std::fprintf(_file, "%c", (c >= 32 && c < 127) ? c : '.');
            }

            std::fprintf(_file, "|\n");
        }

        if (level >= LogLevel::WARN) {
            std::fflush(_file);
        }
    }

private:
    Logger()
        : _global_level(LogLevel::INFO)
        , _file(stderr)
        , _show_timestamp(true)
        , _use_color(true)
        , _start_time(std::chrono::steady_clock::now())
    {
        for (size_t i = 0; i < static_cast<size_t>(LogModule::MAX_MODULES); ++i) {
            _module_levels[i].store(LogLevel::TRACE, std::memory_order_relaxed);
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ─── 颜色 ───

    static constexpr const char* RESET    = "\033[0m";
    static constexpr const char* RED      = "\033[31m";
    static constexpr const char* YELLOW   = "\033[33m";
    static constexpr const char* GREEN    = "\033[32m";
    static constexpr const char* CYAN     = "\033[36m";
    static constexpr const char* GRAY     = "\033[90m";
    static constexpr const char* BOLD_RED = "\033[1;31m";

    static const char* level_color(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return GRAY;
            case LogLevel::DEBUG: return CYAN;
            case LogLevel::INFO:  return GREEN;
            case LogLevel::WARN:  return YELLOW;
            case LogLevel::ERROR: return RED;
            case LogLevel::FATAL: return BOLD_RED;
            default: return RESET;
        }
    }

    static const char* level_name(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "?????";
        }
    }

    static const char* module_color(LogModule module) {
        switch (module) {
            // 物理层/链路层 (深灰/白)：像背景一样，平时不关注，只看状态
            case LogModule::HAL:  return "\033[90m";  // Gray (Bright Black)

            // 网络层 (青色/蓝色系)：核心基础设施
            case LogModule::ARP:  return "\033[36m";  // Cyan
            case LogModule::IPv4: return "\033[34m";  // Blue
            case LogModule::ICMP: return "\033[96m";  // Bright Cyan (与IPv4区分)

            // 传输层 (鲜艳的对比色)：这是你最常调试的"修罗场"
            case LogModule::UDP:  return "\033[32m";  // Green (UDP通常简单、快速)
            case LogModule::TCP:  return "\033[31m";  // Red (TCP逻辑最重，红色醒目)

            // 应用层协议 (高亮度色系)：业务逻辑
            case LogModule::DNS:  return "\033[33m";  // Yellow (查询类，用黄色)
            case LogModule::HTTP: return "\033[35m";  // Magenta (HTTP报文通常很长)

            // 顶层应用 (纯白或加粗)：你写的业务逻辑
            case LogModule::APP:  return "\033[1;37m"; // Bold White (最显眼)
            case LogModule::AI:   return "\033[1;36m"; // Bold Cyan (AI 模块通常代表"高科技"，青色很合适)
            case LogModule::FW:   return "\033[1;33m"; // Bold Yellow (防火墙，安全相关)

            default: return RESET;
        }
    }

    static const char* module_name(LogModule module) {
        switch (module) {
            case LogModule::HAL:  return "HAL ";
            case LogModule::IPv4: return "IPv4";
            case LogModule::ICMP: return "ICMP";
            case LogModule::ARP:  return "ARP ";
            case LogModule::UDP:  return "UDP ";
            case LogModule::TCP:  return "TCP ";
            case LogModule::HTTP: return "HTTP";
            case LogModule::DNS:  return "DNS ";
            case LogModule::APP:  return "APP ";
            case LogModule::AI:   return "AI  ";
            case LogModule::FW:   return "FW  ";
            default: return "????";
        }
    }

    // ─── 成员 ───

    std::atomic<LogLevel> _global_level;
    std::atomic<LogLevel> _module_levels[static_cast<size_t>(LogModule::MAX_MODULES)];
    FILE* _file;
    bool _show_timestamp;
    bool _use_color;
    std::chrono::steady_clock::time_point _start_time;
    std::mutex _mutex;
};

// ============================================================================
// 便捷宏 - 先检查再求值参数
// ============================================================================

#ifdef NEUSTACK_LOG_DISABLE
    // 完全禁用 - 编译期零开销
    #define LOG_TRACE(module, ...) ((void)0)
    #define LOG_DEBUG(module, ...) ((void)0)
    #define LOG_INFO(module, ...)  ((void)0)
    #define LOG_WARN(module, ...)  ((void)0)
    #define LOG_ERROR(module, ...) ((void)0)
    #define LOG_FATAL(module, ...) ((void)0)
    #define LOG_HEXDUMP(module, level, data, len) ((void)0)
#else
    // 运行时检查 - 不满足条件时参数不求值
    #define LOG_TRACE(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::TRACE)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::TRACE, __VA_ARGS__); \
        } while(0)

    #define LOG_DEBUG(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::DEBUG)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::DEBUG, __VA_ARGS__); \
        } while(0)

    #define LOG_INFO(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::INFO)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::INFO, __VA_ARGS__); \
        } while(0)

    #define LOG_WARN(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::WARN)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::WARN, __VA_ARGS__); \
        } while(0)

    #define LOG_ERROR(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::ERROR)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::ERROR, __VA_ARGS__); \
        } while(0)

    #define LOG_FATAL(module, ...) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::FATAL)) \
                neustack::Logger::instance().log_impl( \
                    neustack::LogModule::module, neustack::LogLevel::FATAL, __VA_ARGS__); \
        } while(0)

    #define LOG_HEXDUMP(module, level, data, len) \
        do { \
            if (neustack::Logger::instance().should_log( \
                    neustack::LogModule::module, neustack::LogLevel::level)) \
                neustack::Logger::instance().hexdump_impl( \
                    neustack::LogModule::module, neustack::LogLevel::level, data, len, "  "); \
        } while(0)
#endif

}  // namespace neustack

#endif // NEUSTACK_COMMON_LOG_HPP
