# Extra 1: 日志系统

在深入TCP实现之前，我们需要一个好的日志系统。网络协议栈调试非常依赖日志——数据包转瞬即逝，没有日志就像盲人摸象。

## 1. 需求分析

用户态协议栈的日志需求与普通应用不同：

### 1.1 高性能要求
- 协议栈处理数据包的速度可能达到每秒数万个
- 日志不能成为瓶颈
- 需要能在运行时开关，禁用时零开销

### 1.2 分层调试
- 不同协议层需要独立的日志开关
- 调试TCP时不想看到大量的IP层日志
- 需要按模块过滤

### 1.3 丰富的上下文
- 网络包是二进制数据，需要hex dump
- IP地址、端口需要格式化显示
- 时间戳对于分析时序问题很重要

### 1.4 轻量级
- 不依赖外部库
- 头文件实现，方便内联优化
- 简单易用的API

## 2. 设计方案

```
┌─────────────────────────────────────────────────────────────┐
│                        Logger                                │
├─────────────────────────────────────────────────────────────┤
│  Level: TRACE < DEBUG < INFO < WARN < ERROR < FATAL         │
├─────────────────────────────────────────────────────────────┤
│  Modules:                                                    │
│    HAL   - 硬件抽象层 (设备操作)                              │
│    IPv4  - IP层                                              │
│    ICMP  - ICMP协议                                          │
│    ARP   - ARP协议                                           │
│    UDP   - UDP协议                                           │
│    TCP   - TCP协议                                           │
│    APP   - 应用层                                            │
├─────────────────────────────────────────────────────────────┤
│  Output:                                                     │
│    - Console (stderr)                                        │
│    - File (可选)                                             │
│    - Custom callback (可选)                                  │
└─────────────────────────────────────────────────────────────┘
```

## 3. 日志级别

| 级别 | 用途 | 示例 |
|------|------|------|
| TRACE | 最详细，包括每个包的hex dump | `TRACE: packet hex dump...` |
| DEBUG | 调试信息，包解析细节 | `DEBUG: TCP seq=1234, ack=5678` |
| INFO | 正常运行信息 | `INFO: TCP connection established` |
| WARN | 警告，可恢复的问题 | `WARN: checksum mismatch, dropping` |
| ERROR | 错误，操作失败 | `ERROR: bind() failed: port in use` |
| FATAL | 致命错误，需要终止 | `FATAL: out of memory` |

## 4. 实现

### 4.1 头文件 `include/neustack/common/log.hpp`

```cpp
#ifndef NEUSTACK_COMMON_LOG_HPP
#define NEUSTACK_COMMON_LOG_HPP

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <atomic>

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
    APP  = 6,
    MAX_MODULES
};

// ============================================================================
// Logger 类
// ============================================================================

class Logger {
public:
    // 单例访问
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // 设置全局日志级别
    void set_level(LogLevel level) {
        _global_level = level;
    }

    LogLevel level() const {
        return _global_level;
    }

    // 设置模块日志级别
    void set_module_level(LogModule module, LogLevel level) {
        if (static_cast<size_t>(module) < static_cast<size_t>(LogModule::MAX_MODULES)) {
            _module_levels[static_cast<size_t>(module)] = level;
        }
    }

    LogLevel module_level(LogModule module) const {
        if (static_cast<size_t>(module) < static_cast<size_t>(LogModule::MAX_MODULES)) {
            return _module_levels[static_cast<size_t>(module)];
        }
        return LogLevel::OFF;
    }

    // 检查是否应该记录
    bool should_log(LogModule module, LogLevel level) const {
        if (level < _global_level) return false;
        auto mod_level = _module_levels[static_cast<size_t>(module)];
        return level >= mod_level;
    }

    // 启用/禁用时间戳
    void set_timestamp(bool enable) { _show_timestamp = enable; }
    bool timestamp() const { return _show_timestamp; }

    // 启用/禁用颜色
    void set_color(bool enable) { _use_color = enable; }
    bool color() const { return _use_color; }

    // 设置输出文件
    void set_file(FILE* file) {
        std::lock_guard<std::mutex> lock(_mutex);
        _file = file;
    }

    // 核心日志函数
    void log(LogModule module, LogLevel level, const char* fmt, ...) {
        if (!should_log(module, level)) return;

        std::lock_guard<std::mutex> lock(_mutex);

        // 时间戳
        if (_show_timestamp) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _start_time).count();
            std::fprintf(_file, "[%6lld.%03lld] ", ms / 1000, ms % 1000);
        }

        // 级别和模块
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
        std::fflush(_file);
    }

    // Hex dump 辅助函数
    void hexdump(LogModule module, LogLevel level,
                 const uint8_t* data, size_t len, const char* prefix = "") {
        if (!should_log(module, level)) return;

        std::lock_guard<std::mutex> lock(_mutex);

        for (size_t i = 0; i < len; i += 16) {
            std::fprintf(_file, "%s%04zx: ", prefix, i);

            // Hex 部分
            for (size_t j = 0; j < 16; ++j) {
                if (i + j < len) {
                    std::fprintf(_file, "%02x ", data[i + j]);
                } else {
                    std::fprintf(_file, "   ");
                }
                if (j == 7) std::fprintf(_file, " ");
            }

            std::fprintf(_file, " |");

            // ASCII 部分
            for (size_t j = 0; j < 16 && i + j < len; ++j) {
                char c = static_cast<char>(data[i + j]);
                std::fprintf(_file, "%c", (c >= 32 && c < 127) ? c : '.');
            }

            std::fprintf(_file, "|\n");
        }
        std::fflush(_file);
    }

private:
    Logger()
        : _global_level(LogLevel::INFO)
        , _file(stderr)
        , _show_timestamp(true)
        , _use_color(true)
        , _start_time(std::chrono::steady_clock::now())
    {
        // 默认所有模块使用全局级别
        for (size_t i = 0; i < static_cast<size_t>(LogModule::MAX_MODULES); ++i) {
            _module_levels[i] = LogLevel::TRACE;  // 模块级别默认最低，由全局级别控制
        }
    }

    // 颜色代码
    static constexpr const char* RESET  = "\033[0m";
    static constexpr const char* RED    = "\033[31m";
    static constexpr const char* YELLOW = "\033[33m";
    static constexpr const char* GREEN  = "\033[32m";
    static constexpr const char* CYAN   = "\033[36m";
    static constexpr const char* GRAY   = "\033[90m";
    static constexpr const char* BOLD_RED = "\033[1;31m";

    const char* level_color(LogLevel level) const {
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

    const char* level_name(LogLevel level) const {
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

    const char* module_color(LogModule module) const {
        switch (module) {
            case LogModule::HAL:  return "\033[35m";  // Magenta
            case LogModule::IPv4: return "\033[34m";  // Blue
            case LogModule::ICMP: return "\033[36m";  // Cyan
            case LogModule::ARP:  return "\033[33m";  // Yellow
            case LogModule::UDP:  return "\033[32m";  // Green
            case LogModule::TCP:  return "\033[31m";  // Red
            case LogModule::APP:  return "\033[37m";  // White
            default: return RESET;
        }
    }

    const char* module_name(LogModule module) const {
        switch (module) {
            case LogModule::HAL:  return "HAL ";
            case LogModule::IPv4: return "IPv4";
            case LogModule::ICMP: return "ICMP";
            case LogModule::ARP:  return "ARP ";
            case LogModule::UDP:  return "UDP ";
            case LogModule::TCP:  return "TCP ";
            case LogModule::APP:  return "APP ";
            default: return "????";
        }
    }

    std::atomic<LogLevel> _global_level;
    LogLevel _module_levels[static_cast<size_t>(LogModule::MAX_MODULES)];
    FILE* _file;
    bool _show_timestamp;
    bool _use_color;
    std::chrono::steady_clock::time_point _start_time;
    std::mutex _mutex;
};

// ============================================================================
// 便捷宏 - 零开销关闭
// ============================================================================

#ifdef NEUSTACK_LOG_DISABLE
    // 完全禁用日志，编译器会优化掉所有日志调用
    #define LOG_TRACE(module, ...) ((void)0)
    #define LOG_DEBUG(module, ...) ((void)0)
    #define LOG_INFO(module, ...)  ((void)0)
    #define LOG_WARN(module, ...)  ((void)0)
    #define LOG_ERROR(module, ...) ((void)0)
    #define LOG_FATAL(module, ...) ((void)0)
    #define LOG_HEXDUMP(module, level, data, len) ((void)0)
#else
    #define LOG_TRACE(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::TRACE, __VA_ARGS__)
    #define LOG_DEBUG(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::DEBUG, __VA_ARGS__)
    #define LOG_INFO(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::INFO, __VA_ARGS__)
    #define LOG_WARN(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::WARN, __VA_ARGS__)
    #define LOG_ERROR(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::ERROR, __VA_ARGS__)
    #define LOG_FATAL(module, ...) \
        neustack::Logger::instance().log(neustack::LogModule::module, neustack::LogLevel::FATAL, __VA_ARGS__)
    #define LOG_HEXDUMP(module, level, data, len) \
        neustack::Logger::instance().hexdump(neustack::LogModule::module, neustack::LogLevel::level, data, len, "  ")
#endif

}  // namespace neustack

#endif // NEUSTACK_COMMON_LOG_HPP
```

## 5. 使用示例

### 5.1 基本用法

```cpp
#include "neustack/common/log.hpp"

using namespace neustack;

// 设置全局日志级别
Logger::instance().set_level(LogLevel::DEBUG);

// 各种级别的日志
LOG_TRACE(IPv4, "parsing packet, len=%zu", len);
LOG_DEBUG(TCP, "seq=%u, ack=%u, flags=0x%02x", seq, ack, flags);
LOG_INFO(UDP, "bound to port %u", port);
LOG_WARN(ICMP, "checksum mismatch");
LOG_ERROR(HAL, "device open failed: %s", strerror(errno));
LOG_FATAL(APP, "out of memory");

// Hex dump
LOG_HEXDUMP(IPv4, DEBUG, packet_data, packet_len);
```

### 5.2 模块级别控制

```cpp
// 只看 TCP 的调试信息，其他模块只看警告以上
Logger::instance().set_level(LogLevel::WARN);
Logger::instance().set_module_level(LogModule::TCP, LogLevel::TRACE);
```

### 5.3 输出配置

```cpp
// 禁用颜色 (重定向到文件时)
Logger::instance().set_color(false);

// 输出到文件
FILE* logfile = fopen("neustack.log", "w");
Logger::instance().set_file(logfile);

// 禁用时间戳
Logger::instance().set_timestamp(false);
```

## 6. 输出示例

终端彩色输出：
```
[    0.001] [DEBUG] IPv4: received packet, src=192.168.100.1, dst=192.168.100.2
[    0.001] [DEBUG] TCP : SYN received, seq=1234567890
[    0.002] [INFO ] TCP : connection established
[    0.105] [WARN ] TCP : retransmission timeout, seq=1234567891
[    0.210] [ERROR] TCP : connection reset by peer
```

Hex dump 输出：
```
  0000: 45 00 00 3c 1c 46 40 00  40 06 b1 e6 ac 10 0a 63  |E..<.F@.@......c|
  0010: ac 10 0a 0c 00 50 e6 21  00 00 00 00 a0 02 fa f0  |.....P.!........|
  0020: 14 6c 00 00 02 04 05 b4  04 02 08 0a 01 23 45 67  |.l...........#Eg|
```

## 7. 性能考虑

### 7.1 编译期禁用
```cmake
# 发布版本完全禁用日志
target_compile_definitions(neustack_lib PRIVATE NEUSTACK_LOG_DISABLE)
```

### 7.2 运行时检查
`should_log()` 函数在日志被禁用时会快速返回，避免格式化字符串的开销：

```cpp
void log(...) {
    if (!should_log(module, level)) return;  // 快速路径
    // ... 实际日志逻辑
}
```

### 7.3 避免在热路径上使用

对于每个包都会执行的代码，使用 TRACE 级别，默认不开启：

```cpp
// 热路径 - 使用 TRACE
LOG_TRACE(IPv4, "packet %u bytes", len);

// 重要事件 - 使用 INFO
LOG_INFO(TCP, "connection established");
```

## 8. 集成到现有代码

替换现有的 `std::printf` 调用：

```cpp
// 之前
std::printf("UDP: %s:%u -> %s:%u\n", ...);

// 之后
LOG_DEBUG(UDP, "%s:%u -> %s:%u", ...);
```

## 9. 练习

1. 将日志系统集成到 UDP 模块
2. 在 IPv4 模块中使用日志替换 printf
3. 实现一个命令行参数来控制日志级别
4. 添加日志文件轮转功能（可选）

## 10. 下一步

日志系统就绪后，我们就有了调试 TCP 的利器。TCP 状态机复杂，没有好的日志系统几乎无法调试。

下一章我们将开始 TCP 的实现——用户态协议栈最具挑战性的部分。
