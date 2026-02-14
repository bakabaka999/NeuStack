/**
 * @file main.cpp
 * @brief NeuStack Demo Application
 *
 * 演示 NeuStack 用户态协议栈的完整功能。
 * 使用 NeuStack 门面类，展示库的标准调用方式。
 */

#include "neustack/neustack.hpp"
#include "neustack/metrics/global_metrics.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/ai_agent.hpp"
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <chrono>
#include <poll.h>
#include <unistd.h>

using namespace neustack;

// ============================================================================
// 全局状态
// ============================================================================

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

// ============================================================================
// 命令行参数
// ============================================================================

struct Config {
    std::string local_ip = "192.168.100.2";
    std::string dns_server = "8.8.8.8";
    LogLevel log_level = LogLevel::INFO;
    bool color = true;
    bool timestamp = true;
    bool collect_data = false;
    std::string output_dir = ".";
    std::string model_dir;     // AI 模型目录，空 = 不启用
    bool security_collect = false;
    int  security_label = 0;
};

static void print_usage(const char *prog) {
    std::printf("NeuStack - User-space TCP/IP Stack Demo\n\n");
    std::printf("Usage: %s [options]\n\n", prog);
    std::printf("Options:\n");
    std::printf("  --ip <addr>         Local IP (default: 192.168.100.2)\n");
    std::printf("  --dns <addr>        DNS server (default: 8.8.8.8)\n");
    std::printf("  --models <dir>      AI model directory (enables AI)\n");
    std::printf("  --collect           Enable data collection (CSV)\n");
    std::printf("  --output-dir <dir>  Output directory (default: .)\n");
    std::printf("  --security-collect  Enable security data collection\n");
    std::printf("  --security-label N  Security label: 0=normal, 1=anomaly\n");
    std::printf("  -v / -vv / -q      Verbose / Trace / Quiet\n");
    std::printf("  --no-color          Disable colored output\n");
    std::printf("  --no-time           Disable timestamps\n");
    std::printf("  -h, --help          Show this help\n");
}

static bool parse_args(int argc, char *argv[], Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            cfg.local_ip = argv[++i];
        } else if (std::strcmp(argv[i], "--dns") == 0 && i + 1 < argc) {
            cfg.dns_server = argv[++i];
        } else if (std::strcmp(argv[i], "--models") == 0 && i + 1 < argc) {
            cfg.model_dir = argv[++i];
        } else if (std::strcmp(argv[i], "-v") == 0) {
            cfg.log_level = LogLevel::DEBUG;
        } else if (std::strcmp(argv[i], "-vv") == 0) {
            cfg.log_level = LogLevel::TRACE;
        } else if (std::strcmp(argv[i], "-q") == 0) {
            cfg.log_level = LogLevel::WARN;
        } else if (std::strcmp(argv[i], "--no-color") == 0) {
            cfg.color = false;
        } else if (std::strcmp(argv[i], "--no-time") == 0) {
            cfg.timestamp = false;
        } else if (std::strcmp(argv[i], "--collect") == 0) {
            cfg.collect_data = true;
        } else if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            cfg.output_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--security-collect") == 0) {
            cfg.security_collect = true;
        } else if (std::strcmp(argv[i], "--security-label") == 0 && i + 1 < argc) {
            cfg.security_label = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

// ============================================================================
// 服务设置
// ============================================================================

static void setup_echo_services(NeuStack &stack) {
    // UDP Echo (port 7)
    stack.udp()->bind(7, [&stack](uint32_t src_ip, uint16_t src_port,
                                   const uint8_t *data, size_t len) {
        stack.udp()->sendto(src_ip, src_port, 7, data, len);
    });
    LOG_INFO(UDP, "echo service on port 7");

    // TCP Echo (port 7)
    stack.tcp().listen(7, [](IStreamConnection *conn) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection *c, const uint8_t *d, size_t l) { c->send(d, l); },
            .on_close   = [](IStreamConnection *c) { c->close(); }
        };
    });
    LOG_INFO(TCP, "echo service on port 7");

    // TCP Discard (port 9)
    stack.tcp().listen(9, [](IStreamConnection *) -> StreamCallbacks {
        return {
            .on_receive = [](IStreamConnection *, const uint8_t *, size_t) {},
            .on_close   = [](IStreamConnection *c) { c->close(); }
        };
    });
    LOG_INFO(TCP, "discard service on port 9");
}

static void setup_http_server(NeuStack &stack) {
    auto &server = stack.http_server();
    auto local_ip = stack.ip().local_ip();

    // 首页
    server.get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/html")
            .set_body(
                "<!DOCTYPE html>\n"
                "<html><head><title>NeuStack</title></head>\n"
                "<body>\n"
                "<h1>Welcome to NeuStack!</h1>\n"
                "<p>A high-performance user-space TCP/IP stack.</p>\n"
                "<h2>API</h2>\n"
                "<ul>\n"
                "  <li><a href=\"/api/status\">/api/status</a></li>\n"
                "  <li><a href=\"/api/info\">/api/info</a></li>\n"
                "  <li><a href=\"/api/firewall/status\">/api/firewall/status</a></li>\n"
                "  <li>POST /api/echo</li>\n"
                "</ul>\n"
                "<h2>Download</h2>\n"
                "<ul>\n"
                "  <li><a href=\"/download/1k\">1KB</a>"
                " | <a href=\"/download/100k\">100KB</a>"
                " | <a href=\"/download/1m\">1MB</a>"
                " | <a href=\"/download/10m\">10MB</a>"
                " | <a href=\"/download/100m\">100MB</a></li>\n"
                "</ul>\n"
                "</body></html>\n"
            );
    });

    // Status API
    server.get("/api/status", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"status":"running","version":"1.1.0"})");
    });

    // Info API
    server.get("/api/info", [local_ip](const HttpRequest &) {
        std::string json = R"({"local_ip":")" + ip_to_string(local_ip) + R"(",)";
        json += R"("services":["icmp","udp-echo:7","tcp-echo:7","http:80","dns-client"]})";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    // Firewall Status API
    server.get("/api/firewall/status", [&stack](const HttpRequest &) {
        auto fw = stack.firewall_stats();
        std::string json = "{";
        json += "\"enabled\":" + std::string(stack.firewall_enabled() ? "true" : "false") + ",";
        json += "\"shadow_mode\":" + std::string(stack.firewall_shadow_mode() ? "true" : "false") + ",";
        json += "\"ai_enabled\":" + std::string(stack.firewall_ai_enabled() ? "true" : "false") + ",";
        json += "\"packets_inspected\":" + std::to_string(fw.packets_inspected) + ",";
        json += "\"packets_passed\":" + std::to_string(fw.packets_passed) + ",";
        json += "\"packets_dropped\":" + std::to_string(fw.packets_dropped) + ",";
        json += "\"packets_alerted\":" + std::to_string(fw.packets_alerted);
        if (stack.firewall_ai_enabled()) {
            auto ai = stack.firewall_ai_stats();
            json += ",\"ai\":{";
            json += "\"inferences\":" + std::to_string(ai.inferences_total) + ",";
            json += "\"anomalies\":" + std::to_string(ai.anomalies_detected) + ",";
            json += "\"last_score\":" + std::to_string(ai.last_anomaly_score) + ",";
            json += "\"max_score\":" + std::to_string(ai.max_anomaly_score);
            json += "}";
        }
        json += "}";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    // Echo API
    server.post("/api/echo", [](const HttpRequest &req) {
        return HttpResponse()
            .content_type(req.get_header("Content-Type"))
            .set_body(req.body);
    });

    // Download endpoints
    auto add_download = [&server](const char *path, size_t size) {
        server.get_chunked(path, [size](const HttpRequest &) {
            return std::make_unique<RandomDataGenerator>(size);
        });
    };
    add_download("/download/1k",   1024);
    add_download("/download/100k", 100 * 1024);
    add_download("/download/1m",   1024 * 1024);
    add_download("/download/10m",  10 * 1024 * 1024);
    add_download("/download/100m", 100 * 1024 * 1024);

#ifdef NEUSTACK_AI_ENABLED
    // Agent API
    server.get("/api/agent/status", [&stack](const HttpRequest &) {
        const auto &agent = stack.tcp().agent();
        std::string json = "{";
        json += "\"state\":\"" + std::string(agent_state_name(agent.state())) + "\",";
        json += "\"anomaly_score\":" + std::to_string(agent.anomaly_score()) + ",";
        json += "\"predicted_bw\":" + std::to_string(agent.predicted_bw()) + ",";
        json += "\"alpha\":" + std::to_string(agent.current_alpha()) + ",";
        json += "\"effective_alpha\":" + std::to_string(agent.effective_alpha()) + ",";
        json += "\"accept_connections\":" + std::string(agent.should_accept_connection() ? "true" : "false") + ",";
        json += "\"decisions_total\":" + std::to_string(agent.history().size());
        json += "}";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    server.get("/api/agent/history", [&stack](const HttpRequest &) {
        const auto &history = stack.tcp().agent().history();
        std::string json = "[";
        bool first = true;
        for (const auto &d : history) {
            if (!first) json += ",";
            first = false;
            json += "{\"timestamp_us\":" + std::to_string(d.timestamp_us);
            json += ",\"from\":\"" + std::string(agent_state_name(d.from_state)) + "\"";
            json += ",\"to\":\"" + std::string(agent_state_name(d.to_state)) + "\"";
            json += ",\"reason\":\"" + d.reason + "\"}";
        }
        json += "]";
        return HttpResponse().content_type("application/json").set_body(json);
    });
#endif

    // Firewall status API
    server.get("/api/firewall/status", [&stack](const HttpRequest &) {
        std::string json = "{";
        if (stack.firewall_enabled()) {
            const auto s = stack.firewall_stats();
            json += "\"enabled\":true,";
            json += "\"shadow_mode\":" + std::string(stack.firewall_shadow_mode() ? "true" : "false") + ",";
            json += "\"packets_inspected\":" + std::to_string(s.packets_inspected) + ",";
            json += "\"packets_passed\":" + std::to_string(s.packets_passed) + ",";
            json += "\"packets_dropped\":" + std::to_string(s.packets_dropped) + ",";
            json += "\"packets_alerted\":" + std::to_string(s.packets_alerted) + ",";
            json += "\"ai_enabled\":" + std::string(stack.firewall_ai_enabled() ? "true" : "false");
            if (stack.firewall_ai_enabled()) {
                const auto ai_s = stack.firewall_ai_stats();
                json += ",\"ai\":{";
                json += "\"inferences\":" + std::to_string(ai_s.inferences_total) + ",";
                json += "\"anomalies\":" + std::to_string(ai_s.anomalies_detected) + ",";
                json += "\"last_score\":" + std::to_string(ai_s.last_anomaly_score) + ",";
                json += "\"max_score\":" + std::to_string(ai_s.max_anomaly_score) + ",";
                json += "\"escalations\":" + std::to_string(ai_s.escalations) + ",";
                json += "\"deescalations\":" + std::to_string(ai_s.deescalations);
                json += "}";
            }
        } else {
            json += "\"enabled\":false";
        }
        json += "}";
        return HttpResponse().content_type("application/json").set_body(json);
    });

    server.listen(80);
    LOG_INFO(HTTP, "server on port 80");
}

// ============================================================================
// 交互式命令
// ============================================================================

static void print_help() {
    std::printf("\nCommands:\n");
    std::printf("  d <hostname>  - DNS lookup\n");
    std::printf("  g <ip> <path> - HTTP GET\n");
    std::printf("  m             - Show metrics\n");
    std::printf("  f             - Show firewall stats\n");
    std::printf("  h             - Help\n");
    std::printf("  q             - Quit\n\n");
}

static void handle_command(const std::string &line, NeuStack &stack) {
    if (line.empty()) return;

    if (line[0] == 'g' && line.size() > 2) {
        // 格式: g <ip> <path>
        auto first_space = line.find(' ', 2);
        if (first_space == std::string::npos) {
            std::printf("Usage: g <ip> <path>\n");
            return;
        }
        std::string ip_str = line.substr(2, first_space - 2);
        std::string path = line.substr(first_space + 1);
        uint32_t ip = ip_from_string(ip_str.c_str());
        if (ip == 0) {
            std::printf("Invalid IP: %s\n", ip_str.c_str());
            return;
        }
        LOG_INFO(HTTP, "GET http://%s%s", ip_str.c_str(), path.c_str());
        stack.http_client().get(ip, 80, path, [](const HttpResponse &resp, int err) {
            if (err != 0) {
                LOG_WARN(HTTP, "GET failed (error %d)", err);
                return;
            }
            std::printf("\nHTTP %d %s\n",
                        static_cast<int>(resp.status), http_status_text(resp.status));
            for (const auto &[k, vals] : resp.headers) {
                for (const auto &v : vals) {
                    std::printf("%s: %s\n", k.c_str(), v.c_str());
                }
            }
            std::printf("\n%s\n", resp.body.c_str());
        });
    } else if (line[0] == 'd' && line.size() > 2 && stack.dns()) {
        std::string hostname = line.substr(2);
        LOG_INFO(DNS, "resolving %s...", hostname.c_str());
        stack.dns()->resolve_async(hostname, [hostname](std::optional<DNSResponse> resp) {
            if (!resp || resp->rcode != DNSRcode::NoError) {
                LOG_WARN(DNS, "%s: lookup failed", hostname.c_str());
                return;
            }
            auto ip = resp->get_ip();
            if (ip) LOG_INFO(DNS, "%s -> %s", hostname.c_str(), ip_to_string(*ip).c_str());
            else    LOG_WARN(DNS, "%s: no A record", hostname.c_str());
        });
    } else if (line[0] == 'm') {
        auto snap = global_metrics().snapshot();
        std::printf("\n=== Global Metrics ===\n");
        std::printf("  packets_rx:       %" PRIu64 "\n", snap.packets_rx);
        std::printf("  packets_tx:       %" PRIu64 "\n", snap.packets_tx);
        std::printf("  bytes_rx:         %" PRIu64 "\n", snap.bytes_rx);
        std::printf("  bytes_tx:         %" PRIu64 "\n", snap.bytes_tx);
        std::printf("  syn_received:     %" PRIu64 "\n", snap.syn_received);
        std::printf("  rst_received:     %" PRIu64 "\n", snap.rst_received);
        std::printf("  conn_established: %" PRIu64 "\n", snap.conn_established);
        std::printf("  active_conns:     %" PRIu32 "\n", snap.active_connections);
        std::printf("======================\n\n");
    } else if (line[0] == 'f') {
        auto fw = stack.firewall_stats();
        std::printf("\n=== Firewall Stats ===\n");
        std::printf("  enabled:     %s\n", stack.firewall_enabled() ? "yes" : "no");
        std::printf("  shadow_mode: %s\n", stack.firewall_shadow_mode() ? "ON" : "OFF");
        std::printf("  inspected:   %" PRIu64 "\n", fw.packets_inspected);
        std::printf("  passed:      %" PRIu64 "\n", fw.packets_passed);
        std::printf("  dropped:     %" PRIu64 "\n", fw.packets_dropped);
        std::printf("  alerted:     %" PRIu64 "\n", fw.packets_alerted);
        if (stack.firewall_ai_enabled()) {
            auto ai = stack.firewall_ai_stats();
            std::printf("  --- AI ---\n");
            std::printf("  inferences:  %" PRIu64 "\n", ai.inferences_total);
            std::printf("  anomalies:   %" PRIu64 "\n", ai.anomalies_detected);
            std::printf("  last_score:  %.4f\n", ai.last_anomaly_score);
            std::printf("  max_score:   %.4f\n", ai.max_anomaly_score);
        }
        std::printf("======================\n\n");
    } else if (line[0] == 'h') {
        print_help();
    } else if (line[0] == 'q') {
        g_running = false;
    } else {
        std::printf("Unknown command. Type 'h' for help.\n");
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        return EXIT_SUCCESS;
    }

    // 日志
    auto &logger = Logger::instance();
    logger.set_level(cfg.log_level);
    logger.set_color(cfg.color);
    logger.set_timestamp(cfg.timestamp);

    LOG_INFO(APP, "NeuStack v1.1.0");

    // 信号
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 构建 StackConfig
    StackConfig stack_cfg;
    stack_cfg.local_ip = cfg.local_ip;
    stack_cfg.dns_server = ip_from_string(cfg.dns_server.c_str());
    stack_cfg.log_level = cfg.log_level;

    if (!cfg.model_dir.empty()) {
        stack_cfg.orca_model_path = cfg.model_dir + "/orca_actor.onnx";
        stack_cfg.anomaly_model_path = cfg.model_dir + "/anomaly_detector.onnx";
        stack_cfg.bandwidth_model_path = cfg.model_dir + "/bandwidth_predictor.onnx";
        stack_cfg.security_model_path = cfg.model_dir + "/security_anomaly.onnx";
    }

    if (cfg.collect_data) {
        stack_cfg.data_output_dir = cfg.output_dir;
    }

    stack_cfg.security_label = cfg.security_label;

    if (cfg.security_collect && stack_cfg.data_output_dir.empty()) {
        stack_cfg.data_output_dir = cfg.output_dir;
    }

    // 创建协议栈
    auto stack = NeuStack::create(stack_cfg);
    if (!stack) {
        LOG_FATAL(APP, "Failed to create stack");
        return EXIT_FAILURE;
    }

    // 设置服务
    setup_echo_services(*stack);
    setup_http_server(*stack);

    // 启动信息
    std::printf("\n");
    std::printf("Setup (in another terminal):\n");
    std::printf("  sudo ifconfig %s %s %s up\n",
                stack->device().get_name().c_str(),
                "192.168.100.1", cfg.local_ip.c_str());
    std::printf("\nTest:\n");
    std::printf("  ping %s\n", cfg.local_ip.c_str());
    std::printf("  curl http://%s/\n", cfg.local_ip.c_str());
    std::printf("\n");
    print_help();

    // 手动事件循环（保留交互式命令，不使用 stack->run()）
    std::string cmd_buf;
    uint8_t buf[2048];
    auto last_timer = std::chrono::steady_clock::now();
    auto last_fw_timer = std::chrono::steady_clock::now();
    uint32_t security_tick_count = 0;

    while (g_running) {
        ssize_t n = stack->device().recv(buf, sizeof(buf), 10);
        if (n > 0) {
            // 防火墙检查
            bool pass = stack->firewall_inspect(buf, static_cast<size_t>(n));
            if (pass) {
                stack->ip().on_receive(buf, n);
            }
        }
        stack->http_server().poll();

        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= std::chrono::milliseconds(100)) {
            stack->tcp().on_timer();
            if (stack->dns()) stack->dns()->on_timer();

            // 数据采集
            if (stack->sample_exporter()) stack->sample_exporter()->export_new_samples();
            if (stack->metrics_exporter()) stack->metrics_exporter()->export_delta(100);

            // 安全数据导出 (每秒 flush 一次)
            if (stack->security_exporter()) {
                security_tick_count++;
                if (security_tick_count >= 10) {
                    security_tick_count = 0;
                    stack->security_exporter()->flush(cfg.security_label);
                }
            }

            last_timer = now;
        }

        // 防火墙定时器 (1s)
        if (now - last_fw_timer >= std::chrono::seconds(1)) {
            stack->firewall_on_timer();
            last_fw_timer = now;
        }

        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            if (::read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '\n') {
                    handle_command(cmd_buf, *stack);
                    cmd_buf.clear();
                } else {
                    cmd_buf += ch;
                }
            }
        }
    }

    LOG_INFO(APP, "shutdown");
    return EXIT_SUCCESS;
}
