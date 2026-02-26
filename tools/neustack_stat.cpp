/**
 * neustack-stat — NeuStack 协议栈状态查看工具
 *
 * 独立可执行文件，通过 HTTP API 获取 NeuStack 运行状态。
 * 零依赖 (不链接 neustack 库)，只需标准 C++20 + socket。
 *
 * 编译: g++ -std=c++20 -o neustack-stat neustack_stat.cpp [-lws2_32 on Windows]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <csignal>
#include <functional>

// ────────────────────────────────────────────
// 跨平台 Socket
// ────────────────────────────────────────────

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket

    // Windows 终端 ANSI 支持
    #include <windows.h>
    #include <conio.h>

    static void enable_ansi_terminal() {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hOut, &mode);
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    static bool kbhit_check() { return _kbhit() != 0; }
    static int  getch_char()  { return _getch(); }

#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <poll.h>
    #include <termios.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCKET close

    static void enable_ansi_terminal() {
        // POSIX 终端默认支持 ANSI，无需特殊处理
    }

    // POSIX 非阻塞键盘检测
    static bool kbhit_check() {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        return poll(&pfd, 1, 0) > 0;
    }
    static int getch_char() {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
        return -1;
    }

    // POSIX raw mode (禁用行缓冲，用于 live 模式键盘检测)
    static struct termios orig_termios;
    static bool raw_mode_set = false;

    static void enable_raw_mode() {
        if (raw_mode_set) return;
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO);  // 关闭行缓冲和回显
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_mode_set = true;
    }

    static void disable_raw_mode() {
        if (!raw_mode_set) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        raw_mode_set = false;
    }
#endif

// ────────────────────────────────────────────
// 全局状态 & 信号处理
// ────────────────────────────────────────────

static volatile bool g_running = true;

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

static void setup_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

static void cleanup() {
#ifndef _WIN32
    disable_raw_mode();
#endif
    // 恢复光标 (如果被隐藏)
    printf("\033[?25h");
    fflush(stdout);
}

// ────────────────────────────────────────────
// ANSI 颜色和终端控制
// ────────────────────────────────────────────

namespace ansi {
    static bool color_enabled = true;

    const char* reset()   { return color_enabled ? "\033[0m"  : ""; }
    const char* bold()    { return color_enabled ? "\033[1m"  : ""; }
    const char* dim()     { return color_enabled ? "\033[2m"  : ""; }
    const char* red()     { return color_enabled ? "\033[31m" : ""; }
    const char* green()   { return color_enabled ? "\033[32m" : ""; }
    const char* yellow()  { return color_enabled ? "\033[33m" : ""; }
    const char* blue()    { return color_enabled ? "\033[34m" : ""; }
    const char* cyan()    { return color_enabled ? "\033[36m" : ""; }
    const char* white()   { return color_enabled ? "\033[37m" : ""; }

    void clear_screen() {
        printf("\033[2J\033[H");
        fflush(stdout);
    }

    void hide_cursor() {
        printf("\033[?25l");
        fflush(stdout);
    }

    void show_cursor() {
        printf("\033[?25h");
        fflush(stdout);
    }
}

// ────────────────────────────────────────────
// 数值格式化工具
// ────────────────────────────────────────────

/**
 * 格式化字节数为人类可读形式
 * 1234567 → "1.2 MB"
 */
static std::string format_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024));
    } else if (bytes < 1024ULL * 1024 * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f TB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024));
    }
    return buf;
}

/**
 * 格式化字节速率
 * 5600000.0 → "5.6 MB/s"
 */
static std::string format_rate(double bps) {
    return format_bytes(static_cast<uint64_t>(bps)) + "/s";
}

/**
 * 格式化微秒时间
 * 3200 → "3.2ms"    120 → "120μs"    0.5 → "0.5μs"
 */
static std::string format_us(double us) {
    char buf[32];
    if (us < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.1fns", us * 1000);
    } else if (us < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.0fμs", us);
    } else if (us < 1000000.0) {
        std::snprintf(buf, sizeof(buf), "%.1fms", us / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fs", us / 1000000.0);
    }
    return buf;
}

/**
 * 格式化持续时间（秒）
 * 120 → "2m"    3700 → "1h 1m"    86400 → "1d"
 */
static std::string format_duration(uint64_t seconds) {
    char buf[32];
    if (seconds < 60) {
        std::snprintf(buf, sizeof(buf), "%llus",
                      static_cast<unsigned long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(buf, sizeof(buf), "%llum",
                      static_cast<unsigned long long>(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(buf, sizeof(buf), "%lluh %llum",
                      static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>((seconds % 3600) / 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%llud %lluh",
                      static_cast<unsigned long long>(seconds / 86400),
                      static_cast<unsigned long long>((seconds % 86400) / 3600));
    }
    return buf;
}

/**
 * 格式化大整数 (千分位逗号)
 * 1234567 → "1,234,567"
 */
static std::string format_number(uint64_t n) {
    std::string s = std::to_string(n);
    std::string result;
    int count = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) result = ',' + result;
        result = s[static_cast<size_t>(i)] + result;
        ++count;
    }
    return result;
}

/**
 * 渲染进度条
 * render_bar(0.023, 10) → "██░░░░░░░░"
 */
static std::string render_bar(double fraction, int width) {
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    int filled = static_cast<int>(fraction * width + 0.5);
    std::string bar;
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? "█" : "░";
    }
    return bar;
}

// ────────────────────────────────────────────
// 最小 HTTP GET 客户端
// ────────────────────────────────────────────

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};
static WinsockInit g_winsock_init;
#endif

/**
 * 最小 HTTP/1.1 GET 请求
 *
 * 只支持: GET 方法，Content-Length 响应，非 chunked。
 * 足够用于我们自己控制的 /api/v1/* 端点。
 *
 * @return 响应 body (空字符串表示失败)
 */
static std::string http_get(const std::string& host, uint16_t port,
                             const std::string& path, int timeout_ms = 3000) {
    // 创建 socket
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) return "";

    // 设置超时
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // 连接
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        CLOSE_SOCKET(sock);
        return "";
    }

    // 发送请求
    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Connection: close\r\n"
                          "\r\n";

    auto total = request.size();
    size_t sent = 0;
    while (sent < total) {
        auto n = send(sock, request.data() + sent,
                      static_cast<int>(total - sent), 0);
        if (n <= 0) {
            CLOSE_SOCKET(sock);
            return "";
        }
        sent += static_cast<size_t>(n);
    }

    // 读取响应 (全部读入，因为 Connection: close)
    std::string response;
    response.reserve(8192);
    char buf[4096];
    while (true) {
        auto n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }

    CLOSE_SOCKET(sock);

    // 分离 header 和 body
    auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) return "";

    // 检查 HTTP 状态码
    if (response.size() < 12) return "";
    // "HTTP/1.1 200" → 状态码在位置 9-11
    int status = std::atoi(response.c_str() + 9);
    if (status != 200) return "";

    return response.substr(header_end + 4);
}

// ────────────────────────────────────────────
// 简易 JSON 解析器
// ────────────────────────────────────────────

/**
 * 最小 JSON 值提取器
 *
 * 不是通用 JSON parser，只支持从我们自己的 /api/v1/stats 格式中
 * 提取 key-value 对。支持嵌套 key 查找: "tcp.active_connections"
 *
 * 策略: 用 string::find 定位 key，然后提取后面的值。
 * 对于我们控制的简单 JSON 格式，这比写完整 parser 更实用。
 */
class JsonReader {
public:
    explicit JsonReader(std::string json) : _json(std::move(json)) {}

    /**
     * 提取数值 (整数或浮点)
     * 查找 "key": <number> 模式
     */
    double get_number(const std::string& key, double default_val = 0.0) const {
        auto pos = find_key(key);
        if (pos == std::string::npos) return default_val;

        // 跳过 key 和冒号后的空白
        pos = _json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        ++pos;
        while (pos < _json.size() && (_json[pos] == ' ' || _json[pos] == '\n'
               || _json[pos] == '\r' || _json[pos] == '\t')) ++pos;

        // 解析数值
        char* end;
        double val = std::strtod(_json.c_str() + pos, &end);
        if (end == _json.c_str() + pos) return default_val;
        return val;
    }

    /** 提取字符串值 */
    std::string get_string(const std::string& key,
                           const std::string& default_val = "") const {
        auto pos = find_key(key);
        if (pos == std::string::npos) return default_val;

        pos = _json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        ++pos;
        while (pos < _json.size() && _json[pos] != '"') ++pos;
        if (pos >= _json.size()) return default_val;
        ++pos;  // skip opening "

        std::string result;
        while (pos < _json.size() && _json[pos] != '"') {
            if (_json[pos] == '\\' && pos + 1 < _json.size()) {
                ++pos;  // skip escape
            }
            result.push_back(_json[pos]);
            ++pos;
        }
        return result;
    }

    /** 提取布尔值 */
    bool get_bool(const std::string& key, bool default_val = false) const {
        auto pos = find_key(key);
        if (pos == std::string::npos) return default_val;

        pos = _json.find(':', pos);
        if (pos == std::string::npos) return default_val;
        ++pos;
        while (pos < _json.size() && _json[pos] == ' ') ++pos;

        if (_json.compare(pos, 4, "true") == 0) return true;
        if (_json.compare(pos, 5, "false") == 0) return false;
        return default_val;
    }

    /** 提取 connections 数组中的每个对象 (简化: 按 { } 分割) */
    std::vector<JsonReader> get_array_objects(const std::string& key) const {
        std::vector<JsonReader> result;
        auto pos = find_key(key);
        if (pos == std::string::npos) return result;

        // 找到 [ 开始
        pos = _json.find('[', pos);
        if (pos == std::string::npos) return result;
        ++pos;

        // 逐个提取 {...}
        while (pos < _json.size()) {
            auto obj_start = _json.find('{', pos);
            if (obj_start == std::string::npos) break;

            // 找匹配的 } (简单嵌套计数)
            int depth = 0;
            size_t obj_end = obj_start;
            for (; obj_end < _json.size(); ++obj_end) {
                if (_json[obj_end] == '{') ++depth;
                if (_json[obj_end] == '}') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (depth != 0) break;

            result.emplace_back(
                _json.substr(obj_start, obj_end - obj_start + 1));
            pos = obj_end + 1;
        }
        return result;
    }

    bool empty() const { return _json.empty(); }

private:
    std::string _json;

    /** 查找 "key" 在 JSON 中的位置 */
    size_t find_key(const std::string& key) const {
        std::string needle = "\"" + key + "\"";
        return _json.find(needle);
    }
};

// ────────────────────────────────────────────
// 命令行参数
// ────────────────────────────────────────────

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 80; // Changed default port to 80 to match NeuStack defaults
    bool live = false;
    bool json_output = false;
    bool no_color = false;
    std::string command;       // "", "connections"
    int refresh_interval = 1;  // seconds
};

static Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--live" || arg == "-l") {
            opts.live = true;
        } else if (arg == "--json" || arg == "-j") {
            opts.json_output = true;
        } else if (arg == "--no-color") {
            opts.no_color = true;
        } else if ((arg == "--host") && i + 1 < argc) {
            opts.host = argv[++i];
        } else if ((arg == "--port") && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((arg == "--interval" || arg == "-i") && i + 1 < argc) {
            opts.refresh_interval = std::atoi(argv[++i]);
            if (opts.refresh_interval < 1) opts.refresh_interval = 1;
        } else if (arg == "connections") {
            opts.command = "connections";
        } else if (arg == "--help" || arg == "-h") {
            printf(
                "Usage: neustack-stat [options] [command]\n\n"
                "Commands:\n"
                "  (none)           Show overview status\n"
                "  connections      List active TCP connections\n\n"
                "Options:\n"
                "  --live, -l           Continuous refresh mode\n"
                "  --json, -j           Output raw JSON\n"
                "  --no-color           Disable colored output\n"
                "  --host <addr>        NeuStack host (default: 127.0.0.1)\n"
                "  --port <port>        NeuStack port (default: 80)\n"
                "  --interval, -i <sec> Refresh interval in seconds (default: 1)\n"
                "  --help, -h           Show this help\n"
            );
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\nUse --help for usage.\n",
                    arg.c_str());
            exit(1);
        }
    }
    return opts;
}

// ────────────────────────────────────────────
// 格式化输出: Overview
// ────────────────────────────────────────────

static void print_overview(const JsonReader& j) {
    // 标题
    printf("%s%sNeuStack v1.3.0%s — Status\n",
           ansi::bold(), ansi::cyan(), ansi::reset());
    printf("═══════════════════════════════════════════════════\n\n");

    // Traffic
    printf("%s%sTraffic%s\n", ansi::bold(), ansi::white(), ansi::reset());
    printf("  RX: %s%s%s pkts  %s  (%s%.0f%s pps / %s)\n",
        ansi::green(),
        format_number(static_cast<uint64_t>(j.get_number("packets_rx"))).c_str(),
        ansi::reset(),
        format_bytes(static_cast<uint64_t>(j.get_number("bytes_rx"))).c_str(),
        ansi::yellow(),
        j.get_number("pps_rx"),
        ansi::reset(),
        format_rate(j.get_number("bps_rx")).c_str());
    printf("  TX: %s%s%s pkts  %s  (%s%.0f%s pps / %s)\n",
        ansi::green(),
        format_number(static_cast<uint64_t>(j.get_number("packets_tx"))).c_str(),
        ansi::reset(),
        format_bytes(static_cast<uint64_t>(j.get_number("bytes_tx"))).c_str(),
        ansi::yellow(),
        j.get_number("pps_tx"),
        ansi::reset(),
        format_rate(j.get_number("bps_tx")).c_str());

    printf("\n");

    // TCP
    auto active = static_cast<uint32_t>(j.get_number("active_connections"));
    printf("%s%sTCP Connections:%s %s%u active%s  "
           "(%s established / %s reset / %s timeout)\n",
        ansi::bold(), ansi::white(), ansi::reset(),
        ansi::green(),
        active,
        ansi::reset(),
        format_number(static_cast<uint64_t>(j.get_number("total_established"))).c_str(),
        format_number(static_cast<uint64_t>(j.get_number("total_reset"))).c_str(),
        format_number(static_cast<uint64_t>(j.get_number("total_timeout"))).c_str());

    // RTT
    auto samples = static_cast<uint64_t>(j.get_number("samples"));
    if (samples > 0) {
        printf("  RTT: min=%s  avg=%s  p50=%s  p90=%s  p99=%s%s%s\n",
            format_us(j.get_number("min_us")).c_str(),
            format_us(j.get_number("avg_us")).c_str(),
            format_us(j.get_number("p50_us")).c_str(),
            format_us(j.get_number("p90_us")).c_str(),
            ansi::yellow(),
            format_us(j.get_number("p99_us")).c_str(),
            ansi::reset());
    }

    printf("\n");

    // Security
    bool fw_enabled = j.get_bool("firewall_enabled");
    bool shadow = j.get_bool("shadow_mode");
    printf("%s%sFirewall:%s %s%s%s%s\n",
        ansi::bold(), ansi::white(), ansi::reset(),
        fw_enabled ? ansi::green() : ansi::red(),
        fw_enabled ? "ENABLED" : "DISABLED",
        shadow ? " (shadow mode)" : "",
        ansi::reset());

    if (fw_enabled) {
        printf("  PPS: %.0f  SYN rate: %.1f/s  SYN/SYN-ACK: %.1f  RST ratio: %.1f%%\n",
            j.get_number("pps"),
            j.get_number("syn_rate"),
            j.get_number("syn_synack_ratio"),
            j.get_number("rst_ratio") * 100);
        printf("  Dropped: %s  Alerted: %s\n",
            format_number(static_cast<uint64_t>(j.get_number("packets_dropped"))).c_str(),
            format_number(static_cast<uint64_t>(j.get_number("packets_alerted"))).c_str());
    }

    printf("\n");

    // AI
    auto state = j.get_string("agent_state", "DISABLED");
    auto anomaly = j.get_number("anomaly_score");
    const char* state_color = ansi::green();
    if (state == "DEGRADED") state_color = ansi::yellow();
    if (state == "ATTACK") state_color = ansi::red();
    if (state == "DISABLED") state_color = ansi::dim();

    printf("%s%sAI:%s %s%s%s  anomaly=%s%.3f%s  %s  alpha=%.2f\n",
        ansi::bold(), ansi::white(), ansi::reset(),
        state_color, state.c_str(), ansi::reset(),
        (anomaly > 0.5) ? ansi::red() : ansi::green(),
        anomaly,
        ansi::reset(),
        render_bar(anomaly, 10).c_str(),
        j.get_number("current_alpha"));

    auto orca = j.get_string("orca_status", "disabled");
    auto anom_status = j.get_string("anomaly_status", "disabled");
    auto bw_status = j.get_string("bandwidth_status", "disabled");
    printf("  Models: orca=%s  anomaly=%s  bandwidth=%s\n",
        orca.c_str(), anom_status.c_str(), bw_status.c_str());
}

// ────────────────────────────────────────────
// 格式化输出: Connections
// ────────────────────────────────────────────

static void print_connections(const JsonReader& j) {
    auto count = static_cast<int>(j.get_number("count"));

    printf("%s%sNeuStack — Active TCP Connections (%d)%s\n",
           ansi::bold(), ansi::cyan(), count, ansi::reset());
    printf("═══════════════════════════════════════════════════"
           "═══════════════════════════\n\n");

    // 表头
    printf("%s%-22s %-22s %-14s %-8s %-6s %-10s %-6s%s\n",
        ansi::bold(),
        "LOCAL", "REMOTE", "STATE", "RTT", "CWND", "IN-FLIGHT", "AGE",
        ansi::reset());

    auto conns = j.get_array_objects("connections");
    // 统计各状态数量
    std::unordered_map<std::string, int> state_counts;

    for (const auto& c : conns) {
        auto local = c.get_string("local");
        auto remote = c.get_string("remote");
        auto state = c.get_string("state");
        auto rtt = static_cast<uint32_t>(c.get_number("rtt_us"));
        auto cwnd = static_cast<uint32_t>(c.get_number("cwnd"));
        auto in_flight = static_cast<uint64_t>(c.get_number("bytes_in_flight"));
        auto age = static_cast<uint64_t>(c.get_number("age_seconds"));

        state_counts[state]++;

        const char* state_color = ansi::green();
        if (state == "CLOSE_WAIT" || state == "TIME_WAIT") state_color = ansi::yellow();
        if (state == "SYN_SENT" || state == "SYN_RECEIVED") state_color = ansi::cyan();

        printf("%-22s %-22s %s%-14s%s %-8s %-6u %-10s %s\n",
            local.c_str(),
            remote.c_str(),
            state_color, state.c_str(), ansi::reset(),
            (state == "ESTABLISHED") ? format_us(rtt).c_str() : "--",
            cwnd,
            format_bytes(in_flight).c_str(),
            format_duration(age).c_str());
    }

    // 汇总
    printf("\n%sTotal: %d connections%s (", ansi::bold(), count, ansi::reset());
    bool first = true;
    for (const auto& [state, cnt] : state_counts) {
        if (!first) printf(", ");
        printf("%d %s", cnt, state.c_str());
        first = false;
    }
    printf(")\n");
}

// ────────────────────────────────────────────
// 主函数
// ────────────────────────────────────────────

int main(int argc, char** argv) {
    auto opts = parse_args(argc, argv);

    ansi::color_enabled = !opts.no_color;
    enable_ansi_terminal();
    setup_signal_handlers();
    std::atexit(cleanup);

    // ─── 连接列表子命令 ───
    if (opts.command == "connections") {
        auto body = http_get(opts.host, opts.port,
                             "/api/v1/connections");
        if (body.empty()) {
            fprintf(stderr, "%sError:%s Cannot connect to NeuStack at %s:%u\n"
                    "Is NeuStack running with HTTP server enabled?\n",
                    ansi::red(), ansi::reset(),
                    opts.host.c_str(), opts.port);
            return 1;
        }
        if (opts.json_output) {
            printf("%s\n", body.c_str());
        } else {
            JsonReader j(std::move(body));
            print_connections(j);
        }
        return 0;
    }

    // ─── Live 模式准备 ───
    if (opts.live) {
#ifndef _WIN32
        enable_raw_mode();
#endif
        ansi::hide_cursor();
    }

    // ─── 主循环 ───
    int retry_count = 0;
    constexpr int max_retries = 3;

    do {
        auto body = http_get(opts.host, opts.port,
                             "/api/v1/stats");

        if (body.empty()) {
            if (retry_count < max_retries) {
                retry_count++;
                if (opts.live) {
                    ansi::clear_screen();
                    printf("%sConnecting to %s:%u... (attempt %d/%d)%s\n",
                           ansi::yellow(), opts.host.c_str(), opts.port,
                           retry_count, max_retries, ansi::reset());
                } else {
                    fprintf(stderr,
                        "%sError:%s Cannot connect to NeuStack at %s:%u\n"
                        "Is NeuStack running with HTTP server enabled?\n",
                        ansi::red(), ansi::reset(),
                        opts.host.c_str(), opts.port);
                    return 1;
                }
                std::this_thread::sleep_for(
                    std::chrono::seconds(opts.refresh_interval));
                continue;
            }
            fprintf(stderr, "%sError:%s Failed to connect after %d attempts\n",
                    ansi::red(), ansi::reset(), max_retries);
            return 1;
        }

        retry_count = 0;  // 连接成功，重置计数

        if (opts.json_output) {
            printf("%s\n", body.c_str());
        } else {
            if (opts.live) ansi::clear_screen();
            JsonReader j(std::move(body));
            print_overview(j);

            if (opts.live) {
                printf("\n%s[%s q=quit]%s\n",
                       ansi::dim(),
                       format_duration(static_cast<uint64_t>(
                           opts.refresh_interval)).c_str(),
                       ansi::reset());
                fflush(stdout);
            }
        }

        if (!opts.live) break;

        // 等待刷新间隔，期间检测 'q' 键
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(opts.refresh_interval);
        while (g_running && std::chrono::steady_clock::now() < deadline) {
            if (kbhit_check()) {
                int ch = getch_char();
                if (ch == 'q' || ch == 'Q' || ch == 27 /* ESC */) {
                    g_running = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    } while (g_running && opts.live);

    return 0;
}
