#include "neustack/telemetry/http_endpoints.hpp"
#include "neustack/telemetry/telemetry_api.hpp"
#include "neustack/app/http_server.hpp"
#include "neustack/app/http_types.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/json_builder.hpp"

#include <chrono>

namespace neustack::telemetry {

// ════════════════════════════════════════════
// CORS 头
// ════════════════════════════════════════════

static void add_cors_headers(HttpResponse& resp) {
    resp.set_header("Access-Control-Allow-Origin",  "*");
    resp.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    resp.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    resp.set_header("Access-Control-Max-Age",       "86400");
}

// ════════════════════════════════════════════
// 错误响应
// ════════════════════════════════════════════

static HttpResponse error_response(HttpStatus status, const char* error, const char* message) {
    JsonBuilder b(false);
    b.begin_object();
    b.key("error");   b.write_string(error);                                        b.comma();
    b.key("message"); b.write_string(message);                                      b.comma();
    b.key("status");  b.write_uint64(static_cast<uint64_t>(static_cast<int>(status)));
    b.end_object();
    HttpResponse resp;
    resp.status = status;
    resp.content_type("application/json; charset=utf-8");
    resp.set_body(std::move(b.buf));
    add_cors_headers(resp);
    return resp;
}

// ════════════════════════════════════════════
// 限流器（令牌桶，10 req/s，突发 20）
// ════════════════════════════════════════════

class RateLimiter {
public:
    RateLimiter(double rate, double burst)
        : _rate(rate), _burst(burst), _tokens(burst)
        , _last(std::chrono::steady_clock::now()) {}

    bool try_acquire() {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - _last).count();
        _tokens = std::min(_burst, _tokens + dt * _rate);
        _last = now;
        if (_tokens >= 1.0) { _tokens -= 1.0; return true; }
        return false;
    }

private:
    double _rate, _burst, _tokens;
    std::chrono::steady_clock::time_point _last;
};

// ════════════════════════════════════════════
// 子路由 JSON 序列化
// ════════════════════════════════════════════

static std::string serialize_traffic_json(TelemetryAPI& api, bool pretty) {
    auto t = api.traffic();
    JsonBuilder b(pretty, 256);
    b.begin_object();
    b.key("packets_rx"); b.write_uint64(t.packets_rx); b.comma();
    b.key("packets_tx"); b.write_uint64(t.packets_tx); b.comma();
    b.key("bytes_rx");   b.write_uint64(t.bytes_rx);   b.comma();
    b.key("bytes_tx");   b.write_uint64(t.bytes_tx);   b.comma();
    b.key("pps_rx");     b.write_double(t.pps_rx);     b.comma();
    b.key("pps_tx");     b.write_double(t.pps_tx);     b.comma();
    b.key("bps_rx");     b.write_double(t.bps_rx);     b.comma();
    b.key("bps_tx");     b.write_double(t.bps_tx);
    b.end_object();
    return std::move(b.buf);
}

static std::string serialize_tcp_json(TelemetryAPI& api, bool pretty) {
    auto t = api.tcp_stats();
    JsonBuilder b(pretty, 512);
    b.begin_object();
    b.key("active_connections"); b.write_uint64(t.active_connections); b.comma();
    b.key("total_established");  b.write_uint64(t.total_established);  b.comma();
    b.key("total_reset");        b.write_uint64(t.total_reset);        b.comma();
    b.key("total_timeout");      b.write_uint64(t.total_timeout);      b.comma();
    b.key("total_retransmits");  b.write_uint64(t.total_retransmits);  b.comma();
    b.key("avg_cwnd");           b.write_double(t.avg_cwnd);           b.comma();
    b.key("rtt");
    b.begin_object();
    b.key("min_us");  b.write_double(t.rtt.min_us);  b.comma();
    b.key("avg_us");  b.write_double(t.rtt.avg_us);  b.comma();
    b.key("p50_us");  b.write_double(t.rtt.p50_us);  b.comma();
    b.key("p90_us");  b.write_double(t.rtt.p90_us);  b.comma();
    b.key("p99_us");  b.write_double(t.rtt.p99_us);  b.comma();
    b.key("max_us");  b.write_double(t.rtt.max_us);  b.comma();
    b.key("samples"); b.write_uint64(t.rtt.samples);
    b.end_object();
    b.end_object();
    return std::move(b.buf);
}

static std::string serialize_security_json(TelemetryAPI& api, bool pretty) {
    auto s = api.security_stats();
    JsonBuilder b(pretty, 512);
    b.begin_object();
    b.key("firewall_enabled");        b.write_bool(s.firewall_enabled);                               b.comma();
    b.key("shadow_mode");             b.write_bool(s.shadow_mode);                                    b.comma();
    b.key("ai_enabled");              b.write_bool(s.ai_enabled);                                     b.comma();
    b.key("pps");                     b.write_double(s.pps);                                          b.comma();
    b.key("syn_rate");                b.write_double(s.syn_rate);                                     b.comma();
    b.key("syn_synack_ratio");        b.write_double(s.syn_synack_ratio);                             b.comma();
    b.key("rst_ratio");               b.write_double(s.rst_ratio);                                    b.comma();
    b.key("packets_dropped");         b.write_uint64(s.packets_dropped);                              b.comma();
    b.key("packets_alerted");         b.write_uint64(s.packets_alerted);                              b.comma();
    b.key("anomaly_score");           b.write_double(static_cast<double>(s.anomaly_score));           b.comma();
    b.key("agent_state");             b.write_string(s.agent_state);                                  b.comma();
    b.key("predicted_bandwidth_bps"); b.write_double(static_cast<double>(s.predicted_bandwidth_bps));
    b.end_object();
    return std::move(b.buf);
}

static std::string serialize_connections_json(
    const std::vector<ConnectionDetail>& conns, bool pretty)
{
    auto now = std::chrono::steady_clock::now();
    JsonBuilder b(pretty, conns.size() * 200 + 64);
    b.begin_object();
    b.key("count"); b.write_uint64(conns.size()); b.comma();
    b.key("connections");
    b.begin_array();
    for (size_t i = 0; i < conns.size(); ++i) {
        const auto& c = conns[i];
        if (i > 0) b.comma();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - c.established_at).count();
        b.begin_object();
        b.key("local");            b.write_string(neustack::ip_to_string(c.local_ip)  + ":" + std::to_string(c.local_port));  b.comma();
        b.key("remote");           b.write_string(neustack::ip_to_string(c.remote_ip) + ":" + std::to_string(c.remote_port)); b.comma();
        b.key("state");            b.write_string(c.state);                       b.comma();
        b.key("rtt_us");           b.write_uint64(c.rtt_us);                      b.comma();
        b.key("srtt_us");          b.write_uint64(c.srtt_us);                     b.comma();
        b.key("cwnd");             b.write_uint64(c.cwnd);                        b.comma();
        b.key("bytes_in_flight");  b.write_uint64(c.bytes_in_flight);             b.comma();
        b.key("send_buffer_used"); b.write_uint64(c.send_buffer_used);            b.comma();
        b.key("recv_buffer_used"); b.write_uint64(c.recv_buffer_used);            b.comma();
        b.key("bytes_sent");       b.write_uint64(c.bytes_sent);                  b.comma();
        b.key("bytes_received");   b.write_uint64(c.bytes_received);              b.comma();
        b.key("age_seconds");      b.write_uint64(static_cast<uint64_t>(age));
        b.end_object();
    }
    b.end_array();
    b.end_object();
    return std::move(b.buf);
}

// ════════════════════════════════════════════
// 路由注册入口
// ════════════════════════════════════════════

void register_http_endpoints(HttpServer& server, TelemetryAPI& api) {

    static RateLimiter limiter(10.0, 20.0);

    // ─── Prometheus ───
    server.get("/metrics", [&api](const HttpRequest&) {
        if (!limiter.try_acquire())
            return error_response(HttpStatus::TooManyRequests,
                "Too Many Requests", "Rate limit exceeded. Max 10 req/s.");
        HttpResponse resp;
        resp.content_type("text/plain; version=0.0.4; charset=utf-8");
        resp.set_body(api.to_prometheus());
        add_cors_headers(resp);
        return resp;
    });

    // ─── 完整状态 ───
    server.get("/api/v1/stats", [&api](const HttpRequest& req) {
        if (!limiter.try_acquire())
            return error_response(HttpStatus::TooManyRequests,
                "Too Many Requests", "Rate limit exceeded.");
        bool pretty = req.query_param("pretty") == "true";
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(api.to_json(pretty));
        add_cors_headers(resp);
        return resp;
    });

    // ─── 流量子路由 ───
    server.get("/api/v1/stats/traffic", [&api](const HttpRequest& req) {
        bool pretty = req.query_param("pretty") == "true";
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(serialize_traffic_json(api, pretty));
        add_cors_headers(resp);
        return resp;
    });

    // ─── TCP 子路由 ───
    server.get("/api/v1/stats/tcp", [&api](const HttpRequest& req) {
        bool pretty = req.query_param("pretty") == "true";
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(serialize_tcp_json(api, pretty));
        add_cors_headers(resp);
        return resp;
    });

    // ─── 安全子路由 ───
    server.get("/api/v1/stats/security", [&api](const HttpRequest& req) {
        bool pretty = req.query_param("pretty") == "true";
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(serialize_security_json(api, pretty));
        add_cors_headers(resp);
        return resp;
    });

    // ─── 连接列表 ───
    server.get("/api/v1/connections", [&api](const HttpRequest& req) {
        bool pretty = req.query_param("pretty") == "true";
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(serialize_connections_json(api.connections(), pretty));
        add_cors_headers(resp);
        return resp;
    });

    // ─── 健康检查 ───
    server.get("/api/v1/health", [](const HttpRequest&) {
        HttpResponse resp;
        resp.content_type("application/json; charset=utf-8");
        resp.set_body(R"({"status":"ok","version":"1.4.0"})");
        add_cors_headers(resp);
        return resp;
    });

    // ─── OPTIONS 预检（CORS）+ 兜底 404 ───
    server.set_not_found_handler([](const HttpRequest& req) {
        if (req.method == HttpMethod::OPTIONS) {
            HttpResponse resp;
            resp.status = HttpStatus::NoContent;
            add_cors_headers(resp);
            resp.set_header("Content-Length", "0");
            return resp;
        }
        return error_response(HttpStatus::NotFound, "Not Found", "Unknown endpoint.");
    });
}

} // namespace neustack::telemetry
