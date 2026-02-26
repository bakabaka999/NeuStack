/**
 * @file neustack_demo.cpp
 * @brief NeuStack v1.3.0 — Interactive Demo
 */

#include "neustack/neustack.hpp"
#include "neustack/net/icmp.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/ai_agent.hpp"
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <mutex>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

using namespace neustack;
using namespace std::chrono;

// ============================================================================
// ANSI Colors
// ============================================================================

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_CYAN    "\033[36m"
#define C_BCYAN   "\033[96m"
#define C_GREEN   "\033[32m"
#define C_BGREEN  "\033[92m"
#define C_YELLOW  "\033[33m"
#define C_BYELLOW "\033[93m"
#define C_RED     "\033[31m"
#define C_BRED    "\033[91m"
#define C_BLUE    "\033[34m"
#define C_BBLUE   "\033[94m"
#define C_MAGENTA "\033[35m"
#define C_WHITE   "\033[97m"
#define C_GRAY    "\033[90m"

// ============================================================================
// Global state
// ============================================================================

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

// ─── Ping state ─────────────────────────────────────────────────────────────

struct PingEntry {
    steady_clock::time_point sent_at;
    uint16_t seq;
};

static std::mutex                           g_ping_mutex;
static std::unordered_map<uint16_t, PingEntry> g_ping_pending; // key = seq
static uint16_t                             g_ping_id    = 0;
static uint16_t                             g_ping_seq   = 0;
static std::atomic<uint32_t>               g_ping_recv  {0};
static std::atomic<uint32_t>               g_ping_sent  {0};
static std::atomic<bool>                   g_ping_cancel{false};

// ============================================================================
// Prompt
// ============================================================================

static void print_prompt() {
    // Reset any leaked ANSI state, then print colored prompt
    std::printf(C_RESET C_BCYAN "neustack" C_GRAY "> " C_RESET);
    std::fflush(stdout);
}

/**
 * 用于异步回调中安全打印结果，避免干扰提示符
 */
template<typename... Args>
static void async_printf(const char* format, Args... args) {
    std::printf("\r\033[K"); // 回到行首并清除
    if constexpr (sizeof...(Args) == 0) {
        std::printf("%s", format);
    } else {
        std::printf(format, args...);
    }
    print_prompt();
}

// ============================================================================
// Banner
// ============================================================================

static void print_banner() {
    std::printf("\n");
    std::printf(C_BCYAN C_BOLD
        "███╗   ██╗███████╗██╗   ██╗███████╗████████╗ █████╗  ██████╗██╗  ██╗\n"
        "████╗  ██║██╔════╝██║   ██║██╔════╝╚══██╔══╝██╔══██╗██╔════╝██║ ██╔╝\n"
        "██╔██╗ ██║█████╗  ██║   ██║███████╗   ██║   ███████║██║     █████╔╝ \n"
        "██║╚██╗██║██╔══╝  ██║   ██║╚════██║   ██║   ██╔══██║██║     ██╔═██╗ \n"
        "██║ ╚████║███████╗╚██████╔╝███████║   ██║   ██║  ██║╚██████╗██║  ██╗\n"
        "╚═╝  ╚═══╝╚══════╝ ╚═════╝ ╚══════╝   ╚═╝   ╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝\n"
        C_RESET);
    std::printf(C_GRAY "  User-space TCP/IP Stack  " C_RESET
                C_DIM  "v1.3.0  •  github.com/bakabaka999/NeuStack\n" C_RESET);
    std::printf("\n");
}

// ============================================================================
// Config / args
// ============================================================================

struct Config {
    std::string local_ip   = "192.168.100.2";
    std::string dns_server = "8.8.8.8";
    LogLevel    log_level  = LogLevel::INFO;
    bool color      = true;
    bool timestamp  = true;
    bool collect_data   = false;
    std::string output_dir = ".";
    std::string model_dir;
    bool security_collect = false;
    int  security_label   = 0;
};

static void print_usage(const char *prog) {
    std::printf(C_BOLD "NeuStack Demo" C_RESET " — User-space TCP/IP Stack\n\n");
    std::printf("Usage: %s [options]\n\n", prog);
    std::printf("Options:\n");
    std::printf("  --ip <addr>          Local IP          (default: 192.168.100.2)\n");
    std::printf("  --dns <addr>         DNS server        (default: 8.8.8.8)\n");
    std::printf("  --models <dir>       AI model dir      (enables AI features)\n");
    std::printf("  --collect            Enable CSV data collection\n");
    std::printf("  --output-dir <dir>   Output directory  (default: .)\n");
    std::printf("  --security-collect   Enable security data collection\n");
    std::printf("  --security-label N   Security label: 0=normal 1=anomaly\n");
    std::printf("  -v / -vv / -q        Verbose / Trace / Quiet\n");
    std::printf("  --no-color           Disable colored output\n");
    std::printf("  --no-time            Disable timestamps\n");
    std::printf("  -h, --help           Show this help\n");
}

static bool parse_args(int argc, char *argv[], Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--ip")       == 0 && i+1 < argc) cfg.local_ip   = argv[++i];
        else if (std::strcmp(argv[i], "--dns")      == 0 && i+1 < argc) cfg.dns_server = argv[++i];
        else if (std::strcmp(argv[i], "--models")   == 0 && i+1 < argc) cfg.model_dir  = argv[++i];
        else if (std::strcmp(argv[i], "-v")         == 0) cfg.log_level = LogLevel::DEBUG;
        else if (std::strcmp(argv[i], "-vv")        == 0) cfg.log_level = LogLevel::TRACE;
        else if (std::strcmp(argv[i], "-q")         == 0) cfg.log_level = LogLevel::WARN;
        else if (std::strcmp(argv[i], "--no-color") == 0) cfg.color     = false;
        else if (std::strcmp(argv[i], "--no-time")  == 0) cfg.timestamp = false;
        else if (std::strcmp(argv[i], "--collect")  == 0) cfg.collect_data = true;
        else if (std::strcmp(argv[i], "--output-dir") == 0 && i+1 < argc) cfg.output_dir = argv[++i];
        else if (std::strcmp(argv[i], "--security-collect") == 0) cfg.security_collect = true;
        else if (std::strcmp(argv[i], "--security-label")   == 0 && i+1 < argc)
            cfg.security_label = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

// ============================================================================
// Services
// ============================================================================

static constexpr const char* INDEX_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>NeuStack v1.3.0</title>
    <style>
        :root {
            --bg-color: #0f172a;
            --text-color: #f1f5f9;
            --accent-color: #38bdf8;
            --card-bg: #1e293b;
            --border-color: #334155;
            --hover-color: #7dd3fc;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            line-height: 1.6;
            margin: 0;
            padding: 2rem;
            display: flex;
            justify-content: center;
        }
        .container {
            max-width: 800px;
            width: 100%;
        }
        header {
            text-align: center;
            margin-bottom: 3rem;
        }
        h1 {
            color: var(--accent-color);
            font-size: 2.5rem;
            margin-bottom: 0.5rem;
        }
        .subtitle {
            color: #94a3b8;
            font-size: 1.1rem;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 1.5rem;
        }
        .card {
            background-color: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 0.5rem;
            padding: 1.5rem;
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
        }
        .card h2 {
            margin-top: 0;
            color: var(--accent-color);
            font-size: 1.25rem;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 0.5rem;
            margin-bottom: 1rem;
        }
        ul {
            list-style: none;
            padding: 0;
            margin: 0;
        }
        li {
            margin-bottom: 0.75rem;
        }
        a {
            color: var(--text-color);
            text-decoration: none;
            display: flex;
            align-items: center;
            padding: 0.5rem;
            border-radius: 0.25rem;
            transition: all 0.2s;
            background-color: rgba(255,255,255,0.05);
        }
        a:hover {
            background-color: rgba(56, 189, 248, 0.1);
            color: var(--hover-color);
            transform: translateX(4px);
        }
        .method {
            display: inline-block;
            font-size: 0.75rem;
            font-weight: bold;
            padding: 0.2rem 0.4rem;
            border-radius: 0.25rem;
            margin-right: 0.75rem;
            background-color: #475569;
            min-width: 40px;
            text-align: center;
        }
        .method.get { background-color: #059669; }
        .method.post { background-color: #2563eb; }
        .footer {
            margin-top: 3rem;
            text-align: center;
            color: #64748b;
            font-size: 0.875rem;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>NeuStack v1.3.0</h1>
            <div class="subtitle">High-Performance User-Space TCP/IP Stack</div>
        </header>
        
        <div class="grid">
            <div class="card">
                <h2>Application API</h2>
                <ul>
                    <li><a href="/api/status"><span class="method get">GET</span>/api/status</a></li>
                    <li><a href="/api/info"><span class="method get">GET</span>/api/info</a></li>
                    <li><a href="/api/firewall/status"><span class="method get">GET</span>/api/firewall/status</a></li>
                    <li><a href="#"><span class="method post">POST</span>/api/echo</a></li>
                </ul>
            </div>

            <div class="card">
                <h2>Telemetry API</h2>
                <ul>
                    <li><a href="/api/v1/health"><span class="method get">GET</span>/api/v1/health</a></li>
                    <li><a href="/api/v1/stats"><span class="method get">GET</span>/api/v1/stats</a></li>
                    <li><a href="/api/v1/stats/traffic"><span class="method get">GET</span>/api/v1/stats/traffic</a></li>
                    <li><a href="/api/v1/stats/tcp"><span class="method get">GET</span>/api/v1/stats/tcp</a></li>
                    <li><a href="/api/v1/stats/security"><span class="method get">GET</span>/api/v1/stats/security</a></li>
                    <li><a href="/api/v1/connections"><span class="method get">GET</span>/api/v1/connections</a></li>
                    <li><a href="/metrics"><span class="method get">GET</span>/metrics (Prometheus)</a></li>
                </ul>
            </div>

            <div class="card">
                <h2>Downloads (Speed Test)</h2>
                <ul>
                    <li><a href="/download/1k"><span class="method get">GET</span>1KB Payload</a></li>
                    <li><a href="/download/100k"><span class="method get">GET</span>100KB Payload</a></li>
                    <li><a href="/download/1m"><span class="method get">GET</span>1MB Payload</a></li>
                    <li><a href="/download/10m"><span class="method get">GET</span>10MB Payload</a></li>
                    <li><a href="/download/100m"><span class="method get">GET</span>100MB Payload</a></li>
                </ul>
            </div>
        </div>
        
        <div class="footer">
            Powered by NeuStack &bull; C++20 Asynchronous Networking
        </div>
    </div>
</body>
</html>)HTML";

static void setup_echo_services(NeuStack &stack) {
    // UDP Echo (port 7)
    stack.udp()->bind(7, [&stack](uint32_t src_ip, uint16_t src_port,
                                   const uint8_t *data, size_t len) {
        stack.udp()->sendto(src_ip, src_port, 7, data, len);
    });

    // TCP Echo (port 7)
    stack.tcp().listen(7, [](IStreamConnection *) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection *c, const uint8_t *d, size_t l) { c->send(d, l); },
            .on_close   = [](IStreamConnection *c) { c->close(); }
        };
    });

    // TCP Discard (port 9)
    stack.tcp().listen(9, [](IStreamConnection *) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection *, const uint8_t *, size_t) {},
            .on_close   = [](IStreamConnection *c) { c->close(); }
        };
    });

    LOG_INFO(APP, "Echo services: UDP/TCP port 7, Discard TCP port 9");
}

static void setup_http_server(NeuStack &stack) {
    auto &server   = stack.http_server();
    auto  local_ip = stack.ip().local_ip();

    server.get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/html")
            .set_body(INDEX_HTML);
    });

    server.get("/api/status", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"status":"running","version":"1.3.0"})");
    });

    server.get("/api/info", [local_ip, &stack](const HttpRequest &) {
        std::string json = R"({"local_ip":")" + ip_to_string(local_ip) + R"(",)";
        json += R"("services":["icmp","udp-echo:7","tcp-echo:7","http:80","dns-client")";
        if (stack.firewall_enabled())    json += R"(,"firewall")";
        if (stack.firewall_ai_enabled()) json += R"(,"firewall-ai")";
        if (stack.ai_enabled())          json += R"(,"ai-intelligence")";
        json += "]}";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    server.get("/api/firewall/status", [&stack](const HttpRequest &) {
        std::string json = "{";
        if (stack.firewall_enabled()) {
            const auto s    = stack.firewall_stats();
            auto      *rules = stack.firewall_rules();
            json += "\"enabled\":true,";
            json += "\"shadow_mode\":"  + std::string(stack.firewall_shadow_mode() ? "true" : "false") + ",";
            json += "\"ai_enabled\":"   + std::string(stack.firewall_ai_enabled()  ? "true" : "false") + ",";
            json += "\"packets_inspected\":" + std::to_string(s.packets_inspected) + ",";
            json += "\"packets_passed\":"    + std::to_string(s.packets_passed)    + ",";
            json += "\"packets_dropped\":"   + std::to_string(s.packets_dropped)   + ",";
            json += "\"packets_alerted\":"   + std::to_string(s.packets_alerted);
            if (rules) {
                auto &st = rules->stats();
                json += ",\"rule_engine\":{";
                json += "\"whitelist_hits\":"   + std::to_string(st.whitelist_hits)   + ",";
                json += "\"blacklist_hits\":"   + std::to_string(st.blacklist_hits)   + ",";
                json += "\"rate_limit_drops\":" + std::to_string(st.rate_limit_drops) + ",";
                json += "\"rule_matches\":"     + std::to_string(st.rule_matches)     + ",";
                json += "\"default_passes\":"   + std::to_string(st.default_passes);
                json += "}";
            }
            if (stack.firewall_ai_enabled()) {
                const auto ai = stack.firewall_ai_stats();
                json += ",\"ai\":{";
                json += "\"inferences\":"  + std::to_string(ai.inferences_total)    + ",";
                json += "\"anomalies\":"   + std::to_string(ai.anomalies_detected)  + ",";
                json += "\"last_score\":"  + std::to_string(ai.last_anomaly_score)  + ",";
                json += "\"max_score\":"   + std::to_string(ai.max_anomaly_score)   + ",";
                json += "\"escalations\":" + std::to_string(ai.escalations)         + ",";
                json += "\"deescalations\":" + std::to_string(ai.deescalations);
                json += "}";
            }
        } else {
            json += "\"enabled\":false";
        }
        json += "}";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    server.post("/api/echo", [](const HttpRequest &req) {
        return HttpResponse()
            .content_type(req.get_header("Content-Type"))
            .set_body(req.body);
    });

    auto add_dl = [&server](const char *path, size_t size) {
        server.get_chunked(path, [size](const HttpRequest &) {
            return std::make_unique<RandomDataGenerator>(size);
        });
    };
    add_dl("/download/1k",   1024);
    add_dl("/download/100k", 100  * 1024);
    add_dl("/download/1m",   1024 * 1024);
    add_dl("/download/10m",  10   * 1024 * 1024);
    add_dl("/download/100m", 100  * 1024 * 1024);

#ifdef NEUSTACK_AI_ENABLED
    server.get("/api/agent/status", [&stack](const HttpRequest &) {
        const auto &agent = stack.tcp().agent();
        std::string json = "{";
        json += "\"state\":\""          + std::string(agent_state_name(agent.state()))  + "\",";
        json += "\"anomaly_score\":"    + std::to_string(agent.anomaly_score())          + ",";
        json += "\"predicted_bw\":"     + std::to_string(agent.predicted_bw())           + ",";
        json += "\"alpha\":"            + std::to_string(agent.current_alpha())          + ",";
        json += "\"effective_alpha\":"  + std::to_string(agent.effective_alpha())        + ",";
        json += "\"accept_connections\":" + std::string(agent.should_accept_connection() ? "true" : "false") + ",";
        json += "\"decisions_total\":"  + std::to_string(agent.history().size());
        json += "}";
        return HttpResponse().content_type("application/json").set_body(json);
    });
#endif

    server.listen(80);
    LOG_INFO(HTTP, "HTTP server on port 80");
}

// ============================================================================
// Help
// ============================================================================

static void print_help() {
    std::printf("\n" C_BOLD C_WHITE "  ┌─ Commands ─────────────────────────────────────────────────────────────┐\n" C_RESET);

    std::printf(C_BOLD C_GRAY   "  │ " C_BCYAN  "Network\n" C_RESET);
    std::printf(C_GRAY          "  │  " C_WHITE  "ping <ip>"       C_RESET "              Send ICMP echo (4 probes)\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "dns <hostname>"  C_RESET "         DNS A-record lookup\n");
    std::printf(C_GRAY          "  │\n");

    std::printf(C_BOLD C_GRAY   "  │ " C_BCYAN  "HTTP Client\n" C_RESET);
    std::printf(C_GRAY          "  │  " C_WHITE  "get <ip> <path>" C_RESET "        HTTP GET  (port 80)\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "post <ip> <path> <body>" C_RESET " HTTP POST (port 80)\n");
    std::printf(C_GRAY          "  │\n");

    std::printf(C_BOLD C_GRAY   "  │ " C_BCYAN  "TCP\n" C_RESET);
    std::printf(C_GRAY          "  │  " C_WHITE  "nc <ip> <port>"  C_RESET "         Open TCP connection, send line\n");
    std::printf(C_GRAY          "  │\n");

    std::printf(C_BOLD C_GRAY   "  │ " C_BCYAN  "Telemetry\n" C_RESET);
    std::printf(C_GRAY          "  │  " C_WHITE  "stats"           C_RESET "                  Traffic + TCP summary\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "conns"           C_RESET "                  Active TCP connections\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "json"            C_RESET "                   Full status (pretty JSON)\n");
    std::printf(C_GRAY          "  │\n");

    std::printf(C_BOLD C_GRAY   "  │ " C_BCYAN  "Firewall\n" C_RESET);
    std::printf(C_GRAY          "  │  " C_WHITE  "fw"              C_RESET "                     Show firewall stats\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw shadow on|off" C_RESET "       Toggle shadow mode\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw bl add|del <ip>" C_RESET "     Blacklist IP\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw wl add|del <ip>" C_RESET "     Whitelist IP\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw rule add <id> <port> [tcp|udp]" C_RESET "\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw rule del <id>" C_RESET "       Remove rule\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw rule list"    C_RESET "           List all rules\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw rate <pps>"   C_RESET "          Rate limit (0=off)\n");
    std::printf(C_GRAY          "  │  " C_WHITE  "fw threshold <v>" C_RESET "       AI anomaly threshold\n");
    std::printf(C_GRAY          "  │\n");

    std::printf(C_BOLD C_GRAY   "  │ " C_WHITE   "h" C_RESET " help   " C_WHITE "q" C_RESET " quit\n");
    std::printf(C_BOLD C_WHITE  "  └────────────────────────────────────────────────────────────────────────┘\n" C_RESET);
    std::printf("\n");
}

// ============================================================================
// Command handlers
// ============================================================================

static void cmd_ping(const std::string &args, NeuStack &stack) {
    if (!stack.icmp()) {
        std::printf(C_RED "  [!] ICMP not enabled\n" C_RESET);
        return;
    }
    if (args.empty()) {
        std::printf(C_YELLOW "  Usage: ping <ip>\n" C_RESET);
        return;
    }
    uint32_t dst = ip_from_string(args.c_str());
    if (dst == 0) {
        std::printf(C_RED "  [!] Invalid IP: %s\n" C_RESET, args.c_str());
        return;
    }

    g_ping_id  = static_cast<uint16_t>(getpid() & 0xFFFF);
    g_ping_recv = 0;
    g_ping_sent = 0;
    g_ping_cancel = false;

    std::printf(C_BOLD "  PING %s — 4 packets " C_DIM "(press Enter to stop)\n" C_RESET,
                args.c_str());

    // Register reply callback once (idempotent)
    stack.icmp()->set_echo_reply_callback(
        [](uint32_t src, uint16_t id, uint16_t seq, uint32_t) {
            // Calculate RTT using our pending table
            uint32_t rtt_us = 0;
            {
                std::lock_guard<std::mutex> lk(g_ping_mutex);
                auto it = g_ping_pending.find(seq);
                if (it != g_ping_pending.end()) {
                    auto now = steady_clock::now();
                    rtt_us = static_cast<uint32_t>(
                        duration_cast<microseconds>(now - it->second.sent_at).count());
                    g_ping_pending.erase(it);
                }
            }
            (void)id;
            g_ping_recv.fetch_add(1);
            if (rtt_us > 0) {
                std::printf(C_BGREEN "  ← reply from %s  seq=%-3u  rtt=%.3f ms\n" C_RESET,
                            ip_to_string(src).c_str(), seq,
                            static_cast<double>(rtt_us) / 1000.0);
            } else {
                std::printf(C_BGREEN "  ← reply from %s  seq=%-3u\n" C_RESET,
                            ip_to_string(src).c_str(), seq);
            }
        }
    );

    // Send 4 probes, 1s apart; event loop runs in background so replies arrive
    for (int i = 0; i < 4 && g_running && !g_ping_cancel; ++i) {
        uint16_t seq = ++g_ping_seq;
        {
            std::lock_guard<std::mutex> lk(g_ping_mutex);
            g_ping_pending[seq] = { steady_clock::now(), seq };
        }
        g_ping_sent.fetch_add(1);
        const char payload[] = "NeuStack";
        stack.icmp()->send_echo_request(dst, g_ping_id, seq,
            reinterpret_cast<const uint8_t*>(payload), sizeof(payload) - 1);
        std::printf(C_DIM "  → seq=%-3u sent\n" C_RESET, seq);

        // Wait ~1s while the event loop ticks
        auto wait_until = steady_clock::now() + seconds(1);
        while (steady_clock::now() < wait_until && g_running && !g_ping_cancel) {
            uint8_t buf[2048];
            ssize_t n = stack.device().recv(buf, sizeof(buf), 5);
            if (n > 0) {
                if (stack.firewall_inspect(buf, static_cast<size_t>(n)))
                    stack.ip().on_receive(buf, n);
            }
            stack.http_server().poll();
#ifndef _WIN32
            // Check stdin: Enter cancels ping, other chars discarded
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char ch;
                if (::read(STDIN_FILENO, &ch, 1) != 1) break;
                if (ch == '\n') { g_ping_cancel = true; break; }
            }
#endif
        }
    }

    uint32_t sent = g_ping_sent.load();
    uint32_t recv = g_ping_recv.load();
    int loss = sent > 0 ? static_cast<int>((sent - recv) * 100 / sent) : 100;
    std::printf(C_BOLD "  --- %s ---  %u sent, %u received, %d%% loss\n" C_RESET,
                args.c_str(), sent, recv, loss);
}

static void cmd_dns(const std::string &args, NeuStack &stack) {
    if (!stack.dns()) {
        std::printf(C_RED "  [!] DNS client not available (UDP disabled?)\n" C_RESET);
        return;
    }
    if (args.empty()) {
        std::printf(C_YELLOW "  Usage: dns <hostname>\n" C_RESET);
        return;
    }
    std::printf(C_DIM "  Resolving %s...\n" C_RESET, args.c_str());
    stack.dns()->resolve_async(args, [host = args](std::optional<DNSResponse> resp) {
        if (!resp || resp->rcode != DNSRcode::NoError) {
            async_printf(C_RED "  [!] %s: lookup failed\n" C_RESET, host.c_str());
        } else {
            auto ip = resp->get_ip();
            if (ip)
                async_printf(C_BGREEN "  %s  →  %s\n" C_RESET, host.c_str(), ip_to_string(*ip).c_str());
            else
                async_printf(C_YELLOW "  %s: no A record\n" C_RESET, host.c_str());
        }
    });
}

static void cmd_get(const std::string &args, NeuStack &stack) {
    // get <ip> <path>
    auto sp = args.find(' ');
    if (sp == std::string::npos) {
        std::printf(C_YELLOW "  Usage: get <ip> <path>\n" C_RESET);
        return;
    }
    std::string ip_str = args.substr(0, sp);
    std::string path   = args.substr(sp + 1);
    uint32_t ip = ip_from_string(ip_str.c_str());
    if (ip == 0) {
        std::printf(C_RED "  [!] Invalid IP: %s\n" C_RESET, ip_str.c_str());
        return;
    }
    std::printf(C_DIM "  GET http://%s%s\n" C_RESET, ip_str.c_str(), path.c_str());
    stack.http_client().get(ip, 80, path, [](const HttpResponse &resp, int err) {
        std::printf("\r\033[K");
        if (err != 0) {
            std::printf(C_RED "  [!] Request failed (err=%d)\n" C_RESET, err);
            print_prompt();
            return;
        }
        int code = static_cast<int>(resp.status);
        const char *color = (code < 300) ? C_BGREEN : (code < 400) ? C_BYELLOW : C_BRED;
        std::printf("%s  HTTP %d %s\n" C_RESET, color, code, http_status_text(resp.status));
        for (const auto &[k, vals] : resp.headers)
            for (const auto &v : vals)
                std::printf(C_DIM "  %s: %s\n" C_RESET, k.c_str(), v.c_str());
        if (!resp.body.empty())
            std::printf("\n%s\n", resp.body.c_str());
        print_prompt();
    });
}

static void cmd_post(const std::string &args, NeuStack &stack) {
    // post <ip> <path> <body>
    auto sp1 = args.find(' ');
    if (sp1 == std::string::npos) { std::printf(C_YELLOW "  Usage: post <ip> <path> <body>\n" C_RESET); return; }
    auto sp2 = args.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) { std::printf(C_YELLOW "  Usage: post <ip> <path> <body>\n" C_RESET); return; }

    std::string ip_str = args.substr(0, sp1);
    std::string path   = args.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string body   = args.substr(sp2 + 1);
    uint32_t ip = ip_from_string(ip_str.c_str());
    if (ip == 0) { std::printf(C_RED "  [!] Invalid IP: %s\n" C_RESET, ip_str.c_str()); return; }

    std::printf(C_DIM "  POST http://%s%s  (%zu bytes)\n" C_RESET,
                ip_str.c_str(), path.c_str(), body.size());
    stack.http_client().post(ip, 80, path, body, "text/plain",
        [](const HttpResponse &resp, int err) {
            std::printf("\r\033[K");
            if (err != 0) { std::printf(C_RED "  [!] Request failed (err=%d)\n" C_RESET, err); print_prompt(); return; }
            int code = static_cast<int>(resp.status);
            const char *color = (code < 300) ? C_BGREEN : (code < 400) ? C_BYELLOW : C_BRED;
            std::printf("%s  HTTP %d %s\n" C_RESET, color, code, http_status_text(resp.status));
            if (!resp.body.empty())
                std::printf("\n%s\n", resp.body.c_str());
            print_prompt();
        });
}

static void cmd_nc(const std::string &args, NeuStack &stack) {
    // nc <ip> <port>
    auto sp = args.find(' ');
    if (sp == std::string::npos) { std::printf(C_YELLOW "  Usage: nc <ip> <port>\n" C_RESET); return; }
    std::string ip_str  = args.substr(0, sp);
    std::string port_str = args.substr(sp + 1);
    uint32_t ip   = ip_from_string(ip_str.c_str());
    uint16_t port = static_cast<uint16_t>(std::strtoul(port_str.c_str(), nullptr, 10));
    if (ip == 0 || port == 0) { std::printf(C_RED "  [!] Invalid ip/port\n" C_RESET); return; }

    std::printf(C_DIM "  Connecting to %s:%u...\n" C_RESET, ip_str.c_str(), port);
    stack.tcp().connect(ip, port,
        [ip_str, port](IStreamConnection *conn, int err) {
            std::printf("\r\033[K");
            if (err || !conn) {
                std::printf(C_RED "  [!] Connect failed (err=%d)\n" C_RESET, err);
                print_prompt();
                return;
            }
            std::printf(C_BGREEN "  Connected to %s:%u\n" C_RESET, ip_str.c_str(), port);
            const char msg[] = "Hello from NeuStack!\r\n";
            conn->send(reinterpret_cast<const uint8_t*>(msg), sizeof(msg) - 1);
            std::printf(C_DIM "  → sent: Hello from NeuStack!\n" C_RESET);
            print_prompt();
        },
        [](IStreamConnection *, const uint8_t *data, size_t len) {
            async_printf(C_BCYAN "  ← received: %.*s\n" C_RESET, static_cast<int>(len), data);
        },
        [](IStreamConnection *c) {
            async_printf(C_GRAY "  Connection closed\n" C_RESET);
            c->close();
        }
    );
}

static void cmd_stats(NeuStack &stack) {
    auto t   = stack.telemetry().traffic();
    auto tcp = stack.telemetry().tcp_stats();

    std::printf("\n" C_BOLD C_WHITE
        "  ┌─ Traffic ──────────────────────────────────────────────────────────────┐\n" C_RESET);
    std::printf("  │  " C_CYAN "RX" C_RESET "  %10" PRIu64 " pkts  %10" PRIu64 " bytes"
                "   " C_DIM "%.1f pps  %.1f B/s\n" C_RESET,
                t.packets_rx, t.bytes_rx, t.pps_rx, t.bps_rx);
    std::printf("  │  " C_YELLOW "TX" C_RESET "  %10" PRIu64 " pkts  %10" PRIu64 " bytes"
                "   " C_DIM "%.1f pps  %.1f B/s\n" C_RESET,
                t.packets_tx, t.bytes_tx, t.pps_tx, t.bps_tx);
    std::printf(C_BOLD C_WHITE
        "  ├─ TCP ──────────────────────────────────────────────────────────────────┤\n" C_RESET);
    std::printf("  │  active=%-6u  established=%-8" PRIu64
                "  resets=%-6" PRIu64 "  retransmits=%" PRIu64 "\n",
                tcp.active_connections, tcp.total_established,
                tcp.total_reset, tcp.total_retransmits);
    std::printf("  │  RTT  min=%.1fms  p50=%.1fms  p99=%.1fms  max=%.1fms  samples=%" PRIu64 "\n",
                tcp.rtt.min_us / 1000.0, tcp.rtt.p50_us / 1000.0,
                tcp.rtt.p99_us / 1000.0, tcp.rtt.max_us / 1000.0,
                tcp.rtt.samples);
    std::printf("  │  avg_cwnd=%.1f\n", tcp.avg_cwnd);
    std::printf(C_BOLD C_WHITE
        "  └────────────────────────────────────────────────────────────────────────┘\n" C_RESET "\n");
}

static void cmd_conns(NeuStack &stack) {
    auto conns = stack.telemetry().connections();
    std::printf("\n" C_BOLD C_WHITE
        "  ┌─ Active Connections (%-3zu) ───────────────────────────────────────────┐\n" C_RESET,
        conns.size());
    if (conns.empty()) {
        std::printf("  │  " C_DIM "(none)\n" C_RESET);
    } else {
        std::printf(C_DIM "  │  %-21s %-21s %-14s %s\n" C_RESET,
                    "Local", "Remote", "State", "RTT");
        for (const auto &c : conns) {
            std::string local  = ip_to_string(c.local_ip)  + ":" + std::to_string(c.local_port);
            std::string remote = ip_to_string(c.remote_ip) + ":" + std::to_string(c.remote_port);
            std::printf("  │  " C_WHITE "%-21s" C_RESET " %-21s " C_BGREEN "%-14s" C_RESET
                        " %.2fms\n",
                        local.c_str(), remote.c_str(), c.state.c_str(),
                        static_cast<double>(c.srtt_us) / 1000.0);
        }
    }
    std::printf(C_BOLD C_WHITE
        "  └────────────────────────────────────────────────────────────────────────┘\n" C_RESET "\n");
}

static void cmd_json(NeuStack &stack) {
    std::printf("\n%s\n\n", stack.status_json(true).c_str());
}

static void cmd_fw_stats(NeuStack &stack) {
    std::printf("\n" C_BOLD C_WHITE
        "  ┌─ Firewall ─────────────────────────────────────────────────────────────┐\n" C_RESET);
    if (!stack.firewall_enabled()) {
        std::printf("  │  " C_GRAY "disabled\n" C_RESET);
    } else {
        auto fw = stack.firewall_stats();
        const char *sm = stack.firewall_shadow_mode() ? C_YELLOW "SHADOW" : C_BGREEN "ENFORCE";
        std::printf("  │  mode=%-20s" C_RESET "  ai=%s\n",
                    sm, stack.firewall_ai_enabled() ? C_BGREEN "on" C_RESET : C_GRAY "off" C_RESET);
        std::printf("  │  inspected=%-10" PRIu64 " passed=%-10" PRIu64
                    " dropped=%-8" PRIu64 " alerted=%" PRIu64 "\n",
                    fw.packets_inspected, fw.packets_passed,
                    fw.packets_dropped, fw.packets_alerted);
        if (stack.firewall_ai_enabled()) {
            auto ai = stack.firewall_ai_stats();
            std::printf("  │  " C_CYAN "AI" C_RESET
                        "  inferences=%-8" PRIu64 " anomalies=%-6" PRIu64
                        " score=%.4f\n",
                        ai.inferences_total, ai.anomalies_detected,
                        ai.last_anomaly_score);
        }
        auto *rules = stack.firewall_rules();
        if (rules) {
            auto &st = rules->stats();
            std::printf("  │  rules=%zu  wl_hits=%" PRIu64 "  bl_hits=%" PRIu64
                        "  rate_drops=%" PRIu64 "\n",
                        rules->rule_count(), st.whitelist_hits,
                        st.blacklist_hits, st.rate_limit_drops);
        }
    }
    std::printf(C_BOLD C_WHITE
        "  └────────────────────────────────────────────────────────────────────────┘\n" C_RESET "\n");
}

static void handle_fw_subcommand(const std::string &sub, NeuStack &stack) {
    auto *rules = stack.firewall_rules();

    if (sub.rfind("shadow ", 0) == 0) {
        bool on = sub.find("on") != std::string::npos;
        stack.firewall_set_shadow_mode(on);
        std::printf("  Shadow mode: %s\n", on ? C_YELLOW "ON (alert only)" C_RESET
                                              : C_BGREEN "OFF (enforce)"   C_RESET);

    } else if (sub.rfind("threshold ", 0) == 0) {
        float t = std::strtof(sub.c_str() + 10, nullptr);
        stack.firewall_set_threshold(t);
        std::printf("  AI threshold → %.4f\n", t);

    } else if (sub.rfind("bl add ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        uint32_t ip = ip_from_string(sub.substr(7).c_str());
        if (!ip) { std::printf(C_RED "  [!] Invalid IP\n" C_RESET); return; }
        rules->add_blacklist_ip(ip);
        std::printf(C_BGREEN "  Blacklisted %s  (total: %zu)\n" C_RESET,
                    sub.substr(7).c_str(), rules->blacklist_size());

    } else if (sub.rfind("bl del ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        uint32_t ip = ip_from_string(sub.substr(7).c_str());
        if (!ip) { std::printf(C_RED "  [!] Invalid IP\n" C_RESET); return; }
        rules->remove_blacklist_ip(ip);
        std::printf("  Removed %s from blacklist  (total: %zu)\n",
                    sub.substr(7).c_str(), rules->blacklist_size());

    } else if (sub == "bl clear") {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        rules->clear_blacklist();
        std::printf("  Blacklist cleared\n");

    } else if (sub.rfind("wl add ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        uint32_t ip = ip_from_string(sub.substr(7).c_str());
        if (!ip) { std::printf(C_RED "  [!] Invalid IP\n" C_RESET); return; }
        rules->add_whitelist_ip(ip);
        std::printf(C_BGREEN "  Whitelisted %s  (total: %zu)\n" C_RESET,
                    sub.substr(7).c_str(), rules->whitelist_size());

    } else if (sub.rfind("wl del ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        uint32_t ip = ip_from_string(sub.substr(7).c_str());
        if (!ip) { std::printf(C_RED "  [!] Invalid IP\n" C_RESET); return; }
        rules->remove_whitelist_ip(ip);
        std::printf("  Removed %s from whitelist  (total: %zu)\n",
                    sub.substr(7).c_str(), rules->whitelist_size());

    } else if (sub == "wl clear") {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        rules->clear_whitelist();
        std::printf("  Whitelist cleared\n");

    } else if (sub.rfind("rule add ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        unsigned id = 0, port = 0;
        char proto_str[8] = {};
        int n = std::sscanf(sub.c_str() + 9, "%u %u %7s", &id, &port, proto_str);
        if (n < 2) { std::printf(C_YELLOW "  Usage: fw rule add <id> <port> [tcp|udp]\n" C_RESET); return; }
        uint8_t proto = 0;
        if (std::strcmp(proto_str, "tcp") == 0) proto = 6;
        else if (std::strcmp(proto_str, "udp") == 0) proto = 17;
        if (rules->add_rule(Rule::block_port(static_cast<uint16_t>(id),
                                              static_cast<uint16_t>(port), proto))) {
            std::printf(C_BGREEN "  Rule #%u: block port %u %s  (rules: %zu)\n" C_RESET,
                        id, port,
                        proto == 6 ? "TCP" : proto == 17 ? "UDP" : "any",
                        rules->rule_count());
        } else {
            std::printf(C_RED "  Failed (max %zu rules)\n" C_RESET, RuleEngine::MAX_RULES);
        }

    } else if (sub.rfind("rule del ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        unsigned id = 0;
        std::sscanf(sub.c_str() + 9, "%u", &id);
        if (rules->remove_rule(static_cast<uint16_t>(id)))
            std::printf("  Removed rule #%u\n", id);
        else
            std::printf(C_RED "  Rule #%u not found\n" C_RESET, id);

    } else if (sub == "rule list") {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        std::printf("\n  WL IPs: %zu   BL IPs: %zu   Rules: %zu\n",
                    rules->whitelist_size(), rules->blacklist_size(), rules->rule_count());
        auto &st = rules->stats();
        std::printf("  wl_hits=%" PRIu64 "  bl_hits=%" PRIu64
                    "  rate_drops=%" PRIu64 "  rule_matches=%" PRIu64 "\n\n",
                    st.whitelist_hits, st.blacklist_hits,
                    st.rate_limit_drops, st.rule_matches);

    } else if (sub.rfind("rate ", 0) == 0) {
        if (!rules) { std::printf(C_RED "  [!] Firewall not available\n" C_RESET); return; }
        unsigned pps = 0;
        std::sscanf(sub.c_str() + 5, "%u", &pps);
        if (pps == 0) {
            rules->rate_limiter().set_enabled(false);
            std::printf("  Rate limiter disabled\n");
        } else {
            rules->rate_limiter().set_rate(pps, pps * 2);
            rules->rate_limiter().set_enabled(true);
            std::printf("  Rate limiter: %u pps  burst=%u\n", pps, pps * 2);
        }

    } else {
        std::printf(C_YELLOW "  Unknown fw subcommand. Type 'h' for help.\n" C_RESET);
    }
}

static void handle_command(const std::string &line, NeuStack &stack) {
    if (line.empty()) return;

    // Split into command + rest
    auto sp = line.find(' ');
    std::string cmd  = (sp == std::string::npos) ? line : line.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? ""   : line.substr(sp + 1);

    if (cmd.empty()) return;

    if      (cmd == "ping")  cmd_ping(rest, stack);
    else if (cmd == "dns")   cmd_dns(rest, stack);
    else if (cmd == "get")   cmd_get(rest, stack);
    else if (cmd == "post")  cmd_post(rest, stack);
    else if (cmd == "nc")    cmd_nc(rest, stack);
    else if (cmd == "stats") cmd_stats(stack);
    else if (cmd == "conns") cmd_conns(stack);
    else if (cmd == "json")  cmd_json(stack);
    else if (cmd == "fw") {
        if (rest.empty())
            cmd_fw_stats(stack);
        else
            handle_fw_subcommand(rest, stack);
    }
    else if (cmd == "h" || cmd == "help") print_help();
    else if (cmd == "q" || cmd == "quit") g_running = false;
    else std::printf(C_YELLOW "  Unknown command '%s'. Type 'h' for help.\n" C_RESET, cmd.c_str());
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return EXIT_SUCCESS;

    auto &logger = Logger::instance();
    logger.set_level(cfg.log_level);
    logger.set_color(cfg.color);
    logger.set_timestamp(cfg.timestamp);

    print_banner();
    LOG_INFO(APP, "NeuStack v1.3.0 starting");

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    StackConfig sc;
    sc.local_ip    = cfg.local_ip;
    sc.dns_server  = ip_from_string(cfg.dns_server.c_str());
    sc.log_level   = cfg.log_level;

    if (!cfg.model_dir.empty()) {
        sc.orca_model_path      = cfg.model_dir + "/orca_actor.onnx";
        sc.anomaly_model_path   = cfg.model_dir + "/anomaly_detector.onnx";
        sc.bandwidth_model_path = cfg.model_dir + "/bandwidth_predictor.onnx";
        sc.security_model_path  = cfg.model_dir + "/security_anomaly.onnx";
    }
    if (cfg.collect_data)
        sc.data_output_dir = cfg.output_dir;
    sc.security_label = cfg.security_label;
    if (cfg.security_collect && sc.data_output_dir.empty())
        sc.data_output_dir = cfg.output_dir;

    auto stack = NeuStack::create(sc);
    if (!stack) { LOG_FATAL(APP, "Failed to create stack"); return EXIT_FAILURE; }

    setup_echo_services(*stack);
    setup_http_server(*stack);

    // Startup info
    std::printf(C_BOLD "  Setup" C_RESET " (run in another terminal):\n");
    std::printf("    sudo ./scripts/nat/setup_nat.sh --dev %s\n\n",
                stack->device().get_name().c_str());
    std::printf(C_BOLD "  Test:\n" C_RESET);
    std::printf("    ping %s\n", cfg.local_ip.c_str());
    std::printf("    curl http://%s/\n\n", cfg.local_ip.c_str());
    std::printf(C_BOLD "  Telemetry:\n" C_RESET);
    std::printf("    curl http://%s/api/v1/health\n",   cfg.local_ip.c_str());
    std::printf("    curl http://%s/api/v1/stats | python3 -m json.tool\n", cfg.local_ip.c_str());
    std::printf("    curl http://%s/metrics\n\n",       cfg.local_ip.c_str());

    print_help();
    print_prompt();

    // ─── Event loop ─────────────────────────────────────────────────────────
    std::string    cmd_buf;
    uint8_t        buf[2048];
    auto last_timer    = steady_clock::now();
    auto last_fw_timer = steady_clock::now();
    uint32_t sec_tick  = 0;

    while (g_running) {
        ssize_t n = stack->device().recv(buf, sizeof(buf), 10);
        if (n > 0) {
            if (stack->firewall_inspect(buf, static_cast<size_t>(n)))
                stack->ip().on_receive(buf, n);
        }
        stack->http_server().poll();

        auto now = steady_clock::now();
        if (now - last_timer >= milliseconds(100)) {
            stack->tcp().on_timer();
            if (stack->dns()) stack->dns()->on_timer();

            if (stack->sample_exporter())  stack->sample_exporter()->export_new_samples();
            if (stack->metrics_exporter()) stack->metrics_exporter()->export_delta(100);

            if (stack->security_exporter()) {
                if (++sec_tick >= 10) {
                    sec_tick = 0;
                    stack->security_exporter()->flush(cfg.security_label);
                }
            }
            last_timer = now;
        }

        if (now - last_fw_timer >= seconds(1)) {
            stack->firewall_on_timer();
            last_fw_timer = now;
        }

        // Non-blocking stdin
#ifdef _WIN32
        if (_kbhit()) {
            char ch = static_cast<char>(_getch());
                if (ch == '\r' || ch == '\n') {
                    std::printf("\n");
                    handle_command(cmd_buf, *stack);
                    cmd_buf.clear();
                    if (g_running) print_prompt();
                } else {
                cmd_buf += ch;
            }
        }
#else
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            if (::read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '\n') {
                    handle_command(cmd_buf, *stack);
                    cmd_buf.clear();
                    if (g_running) print_prompt();
                } else {
                    cmd_buf += ch;
                }
            }
        }
#endif
    }

    LOG_INFO(APP, "shutdown");
    return EXIT_SUCCESS;
}
