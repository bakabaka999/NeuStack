/**
 * @file main.cpp
 * @brief NeuStack Demo Application
 *
 * 演示 NeuStack 用户态协议栈的完整功能：
 * - ICMP: ping 响应
 * - UDP:  Echo 服务
 * - TCP:  Echo 服务
 * - HTTP: Web 服务器
 * - DNS:  域名解析客户端
 */

#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/app/http_server.hpp"
#include "neustack/app/http_client.hpp"
#include "neustack/app/dns_client.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/sample_exporter.hpp"
#include "neustack/metrics/metric_exporter.hpp"

#include <csignal>
#include <memory>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    uint32_t local_ip = 0;
    uint32_t dns_server = 0x08080808;  // 8.8.8.8
    LogLevel log_level = LogLevel::INFO;
    bool color = true;
    bool timestamp = true;
    bool collect_data = false;         // 数据采集开关
    std::string output_dir = ".";      // 数据输出目录
};

static void print_usage(const char *prog) {
    std::printf("NeuStack - User-space TCP/IP Stack Demo\n\n");
    std::printf("Usage: %s [options]\n\n", prog);
    std::printf("Options:\n");
    std::printf("  --ip <addr>     Local IP address (default: 192.168.100.2)\n");
    std::printf("  --dns <addr>    DNS server (default: 8.8.8.8)\n");
    std::printf("  -v              Verbose (DEBUG level)\n");
    std::printf("  -vv             Very verbose (TRACE level)\n");
    std::printf("  -q              Quiet (WARN level)\n");
    std::printf("  --no-color      Disable colored output\n");
    std::printf("  --no-time       Disable timestamps\n");
    std::printf("  --collect       Enable data collection (CSV export)\n");
    std::printf("  --output-dir <dir>  Output directory for CSV files (default: .)\n");
    std::printf("  -h, --help      Show this help\n");
}

static bool parse_args(int argc, char *argv[], Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            cfg.local_ip = ip_from_string(argv[++i]);
        } else if (std::strcmp(argv[i], "--dns") == 0 && i + 1 < argc) {
            cfg.dns_server = ip_from_string(argv[++i]);
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
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return false;
        }
    }

    if (cfg.local_ip == 0) {
        cfg.local_ip = ip_from_string("192.168.100.2");
    }

    return true;
}

// ============================================================================
// 服务设置
// ============================================================================

static void setup_udp_echo(UDPLayer &udp) {
    udp.bind(7, [&udp](uint32_t src_ip, uint16_t src_port,
                        const uint8_t *data, size_t len) {
        LOG_DEBUG(UDP, "echo %s:%u -> %zu bytes",
                  ip_to_string(src_ip).c_str(), src_port, len);
        udp.sendto(src_ip, src_port, 7, data, len);
    });
    LOG_INFO(UDP, "echo service on port 7");
}

static void setup_tcp_echo(TCPLayer &tcp) {
    tcp.listen(7, [](IStreamConnection *conn) -> StreamCallbacks {
        LOG_DEBUG(TCP, "echo connection from %s:%u",
                  ip_to_string(conn->remote_ip()).c_str(), conn->remote_port());
        return StreamCallbacks{
            .on_receive = [](IStreamConnection *conn, const uint8_t *data, size_t len) {
                conn->send(data, len);
            },
            .on_close = [](IStreamConnection *conn) {
                conn->close();
            }
        };
    });
    LOG_INFO(TCP, "echo service on port 7");
}

static void setup_http_server(HttpServer &server, uint32_t local_ip) {
    // 首页
    server.get("/", [](const HttpRequest &req) {
        return HttpResponse()
            .content_type("text/html")
            .set_body(
                "<!DOCTYPE html>\n"
                "<html><head><title>NeuStack</title></head>\n"
                "<body>\n"
                "<h1>Welcome to NeuStack!</h1>\n"
                "<p>A high-performance user-space TCP/IP stack.</p>\n"
                "<h2>API Endpoints</h2>\n"
                "<ul>\n"
                "  <li><a href=\"/api/status\">/api/status</a> - Stack status</li>\n"
                "  <li><a href=\"/api/info\">/api/info</a> - Stack info</li>\n"
                "  <li>POST /api/echo - Echo request body</li>\n"
                "</ul>\n"
                "<h2>Download (for data collection)</h2>\n"
                "<ul>\n"
                "  <li><a href=\"/download/1m\">/download/1m</a> - 1MB random data</li>\n"
                "  <li><a href=\"/download/5m\">/download/5m</a> - 5MB random data</li>\n"
                "  <li><a href=\"/download/10m\">/download/10m</a> - 10MB random data</li>\n"
                "</ul>\n"
                "</body></html>\n"
            );
    });

    // 状态 API
    server.get("/api/status", [](const HttpRequest &req) {
        return HttpResponse()
            .content_type("application/json")
            .set_body(R"({"status":"running","version":"0.1.0"})");
    });

    // 信息 API
    server.get("/api/info", [local_ip](const HttpRequest &req) {
        std::string json = R"({"local_ip":")" + ip_to_string(local_ip) + R"(",)";
        json += R"("services":["icmp","udp-echo:7","tcp-echo:7","http:80","dns-client"]})";
        return HttpResponse()
            .content_type("application/json")
            .set_body(json);
    });

    // Echo API
    server.post("/api/echo", [](const HttpRequest &req) {
        return HttpResponse()
            .content_type(req.get_header("Content-Type"))
            .set_body(req.body);
    });

    // 下载端点 - 用于数据采集（生成指定大小的随机数据）
    // 客户端用 curl -o /dev/null 下载，NeuStack 发送数据触发拥塞控制
    auto download_handler = [](const HttpRequest &req, size_t size_bytes) {
        std::string data(size_bytes, '\0');
        // 用简单伪随机填充（不需要真随机，只是为了产生流量）
        uint32_t state = 0xDEADBEEF;
        for (size_t i = 0; i < size_bytes; i += 4) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            size_t remaining = std::min(size_bytes - i, static_cast<size_t>(4));
            std::memcpy(&data[i], &state, remaining);
        }
        return HttpResponse()
            .content_type("application/octet-stream")
            .set_header("Cache-Control", "no-cache")
            .set_body(data);
    };

    server.get("/download/1m", [download_handler](const HttpRequest &req) {
        return download_handler(req, 1 * 1024 * 1024);
    });
    server.get("/download/5m", [download_handler](const HttpRequest &req) {
        return download_handler(req, 5 * 1024 * 1024);
    });
    server.get("/download/10m", [download_handler](const HttpRequest &req) {
        return download_handler(req, 10 * 1024 * 1024);
    });

    server.listen(80);
    LOG_INFO(HTTP, "server on port 80");
}

// ============================================================================
// 交互式命令处理
// ============================================================================

static void print_help() {
    std::printf("\nCommands:\n");
    std::printf("  d <hostname>  - DNS lookup (e.g., d www.google.com)\n");
    std::printf("  g <ip> <path> - HTTP GET (e.g., g 192.168.100.1 /)\n");
    std::printf("  m             - Show AI metrics\n");
    std::printf("  h             - Show this help\n");
    std::printf("  q             - Quit\n\n");
}

static void handle_command(const std::string &line, DNSClient &dns, HttpClient &http) {
    if (line.empty()) return;

    if (line[0] == 'd' && line.size() > 2) {
        // DNS lookup: d hostname
        std::string hostname = line.substr(2);
        LOG_INFO(DNS, "resolving %s...", hostname.c_str());
        dns.resolve_async(hostname, [hostname](std::optional<DNSResponse> resp) {
            if (!resp) {
                LOG_WARN(DNS, "%s: lookup failed", hostname.c_str());
                return;
            }
            if (resp->rcode != DNSRcode::NoError) {
                LOG_WARN(DNS, "%s: error code %d", hostname.c_str(), static_cast<int>(resp->rcode));
                return;
            }
            auto ip = resp->get_ip();
            if (ip) {
                LOG_INFO(DNS, "%s -> %s", hostname.c_str(), ip_to_string(*ip).c_str());
            } else {
                LOG_WARN(DNS, "%s: no A record", hostname.c_str());
            }
        });
    } else if (line[0] == 'g' && line.size() > 2) {
        // HTTP GET: g ip path
        auto space = line.find(' ', 2);
        if (space == std::string::npos) {
            std::printf("Usage: g <ip> <path>\n");
            return;
        }
        std::string ip_str = line.substr(2, space - 2);
        std::string path = line.substr(space + 1);
        uint32_t ip = ip_from_string(ip_str.c_str());

        LOG_INFO(HTTP, "GET http://%s%s", ip_str.c_str(), path.c_str());
        http.get(ip, 80, path, [](const HttpResponse &resp, int error) {
            if (error != 0) {
                LOG_WARN(HTTP, "request failed: %d", error);
                return;
            }
            LOG_INFO(HTTP, "response: %d %s (%zu bytes)",
                     static_cast<int>(resp.status),
                     http_status_text(resp.status),
                     resp.body.size());
            if (!resp.body.empty() && resp.body.size() < 500) {
                std::printf("%s\n", resp.body.c_str());
            }
        });
    } else if (line[0] == 'm') {
        // 打印 AI 指标采集状态
        auto snap = global_metrics().snapshot();
        std::printf("\n=== Global Metrics ===\n");
        std::printf("  packets_rx:       %" PRIu64 "\n", snap.packets_rx);
        std::printf("  packets_tx:       %" PRIu64 "\n", snap.packets_tx);
        std::printf("  bytes_rx:         %" PRIu64 "\n", snap.bytes_rx);
        std::printf("  bytes_tx:         %" PRIu64 "\n", snap.bytes_tx);
        std::printf("  syn_received:     %" PRIu64 "\n", snap.syn_received);
        std::printf("  rst_received:     %" PRIu64 "\n", snap.rst_received);
        std::printf("  fin_received:     %" PRIu64 "\n", snap.fin_received);
        std::printf("  conn_established: %" PRIu64 "\n", snap.conn_established);
        std::printf("  conn_closed:      %" PRIu64 "\n", snap.conn_closed);
        std::printf("  active_connections: %" PRIu32 "\n", snap.active_connections);
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
// 主循环
// ============================================================================

static void run_event_loop(NetDevice &device, IPv4Layer &ip_layer,
                           TCPLayer &tcp_layer, DNSClient &dns,
                           HttpClient &http, HttpServer &http_server,
                           SampleExporter *sample_exp,
                           MetricsExporter *metrics_exp) {
    uint8_t buf[2048];
    std::string cmd_buf;
    auto last_timer = std::chrono::steady_clock::now();
    constexpr auto TIMER_INTERVAL = std::chrono::milliseconds(100);

    while (g_running) {
        // 收包
        ssize_t n = device.recv(buf, sizeof(buf), 100);
        if (n > 0) {
            ip_layer.on_receive(buf, n);
        } else if (n < 0 && errno != EINTR) {
            LOG_ERROR(HAL, "recv: %s", std::strerror(errno));
            break;
        }

        // HTTP 分段发送：TCP ACK 释放缓冲区空间后继续发送
        http_server.poll();

        // 定时器
        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= TIMER_INTERVAL) {
            tcp_layer.on_timer();
            dns.on_timer();

            // 数据导出 (每 100ms)
            if (sample_exp) {
                sample_exp->export_new_samples();
            }
            if (metrics_exp) {
                metrics_exp->export_delta(100);
            }

            last_timer = now;
        }

        // 键盘输入
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char ch;
            if (::read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '\n') {
                    handle_command(cmd_buf, dns, http);
                    cmd_buf.clear();
                } else {
                    cmd_buf += ch;
                }
            }
        }
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

    // 配置日志
    auto &logger = Logger::instance();
    logger.set_level(cfg.log_level);
    logger.set_color(cfg.color);
    logger.set_timestamp(cfg.timestamp);

    LOG_INFO(APP, "NeuStack v0.1.0");

    // 信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ─── HAL 层 ───
    auto device = NetDevice::create();
    if (!device || device->open() < 0) {
        LOG_FATAL(HAL, "failed to open device");
        return EXIT_FAILURE;
    }
    LOG_INFO(HAL, "device: %s", device->get_name().c_str());

    // ─── 网络层 ───
    IPv4Layer ip_layer(*device);
    ip_layer.set_local_ip(cfg.local_ip);

    ICMPHandler icmp(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &icmp);

    // ─── 传输层 ───
    UDPLayer udp(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::UDP), &udp);

    TCPLayer tcp(ip_layer, cfg.local_ip);
    tcp.set_default_options(TCPOptions::high_throughput());
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &tcp);

    // ─── 应用层 ───
    setup_udp_echo(udp);
    setup_tcp_echo(tcp);

    // TCP Discard 服务 (端口 9) - 只收不回，用于大数据传输测试
    tcp.listen(9, [](IStreamConnection *) -> StreamCallbacks {
        return StreamCallbacks{
            .on_receive = [](IStreamConnection *, const uint8_t *, size_t) {},
            .on_close = [](IStreamConnection *conn) { conn->close(); }
        };
    });
    LOG_INFO(TCP, "discard service on port 9");

    HttpServer http_server(tcp);
    setup_http_server(http_server, cfg.local_ip);

    HttpClient http_client(tcp);

    DNSClient dns(udp, cfg.dns_server);
    dns.init();

    // ─── 数据采集 (可选) ───
    std::unique_ptr<SampleExporter> sample_exporter;
    std::unique_ptr<MetricsExporter> metrics_exporter;

    if (cfg.collect_data) {
        std::string samples_path = cfg.output_dir + "/tcp_samples.csv";
        std::string metrics_path = cfg.output_dir + "/global_metrics.csv";

        sample_exporter = std::make_unique<SampleExporter>(
            samples_path, tcp.metrics_buffer()
        );
        metrics_exporter = std::make_unique<MetricsExporter>(metrics_path);

        LOG_INFO(APP, "data collection enabled");
        LOG_INFO(APP, "  tcp_samples:    %s", samples_path.c_str());
        LOG_INFO(APP, "  global_metrics: %s", metrics_path.c_str());
    }

    // ─── 启动信息 ───
    LOG_INFO(APP, "local IP: %s", ip_to_string(cfg.local_ip).c_str());
    LOG_INFO(APP, "DNS server: %s", ip_to_string(cfg.dns_server).c_str());

    std::printf("\n");
    std::printf("Setup (in another terminal):\n");
    std::printf("  sudo ifconfig %s 192.168.100.1 192.168.100.2 up\n",
                device->get_name().c_str());
    std::printf("\n");
    std::printf("Test:\n");
    std::printf("  ping 192.168.100.2\n");
    std::printf("  curl http://192.168.100.2/\n");
    std::printf("  curl http://192.168.100.2/api/status\n");
    std::printf("\n");
    std::printf("For DNS (requires forwarding from host):\n");
    std::printf("  # Terminal 2: socat UDP4-LISTEN:53,bind=192.168.100.1,fork UDP4:8.8.8.8:53\n");
    std::printf("  # Then run with: ./neustack --dns 192.168.100.1\n");
    std::printf("\n");
    print_help();

    // ─── 主循环 ───
    run_event_loop(*device, ip_layer, tcp, dns, http_client, http_server,
                   sample_exporter.get(), metrics_exporter.get());

    // ─── 关闭前 flush 数据 ───
    if (sample_exporter) {
        sample_exporter->flush();
        LOG_INFO(APP, "exported %zu TCP samples", sample_exporter->exported_count());
    }
    if (metrics_exporter) {
        metrics_exporter->flush();
    }

    LOG_INFO(APP, "shutdown");
    device->close();

    return EXIT_SUCCESS;
}
