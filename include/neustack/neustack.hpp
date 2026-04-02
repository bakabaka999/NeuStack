#ifndef NEUSTACK_NEUSTACK_HPP
#define NEUSTACK_NEUSTACK_HPP

// ─── 协议栈各层 ───
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"
#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/app/http_server.hpp"
#include "neustack/app/http_client.hpp"
#include "neustack/app/dns_client.hpp"

// ─── 防火墙 ───
#include "neustack/firewall/packet_event.hpp"
#include "neustack/firewall/firewall_decision.hpp"
#include "neustack/firewall/rule.hpp"
#include "neustack/firewall/rate_limiter.hpp"
#include "neustack/firewall/rule_engine.hpp"
#include "neustack/firewall/firewall_ai.hpp"
#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/metrics/security_features.hpp"

// ─── TLS ───
#ifdef NEUSTACK_TLS_ENABLED
#include "neustack/tls/tls_context.hpp"
#include "neustack/tls/tls_connection.hpp"
#include "neustack/tls/tls_layer.hpp"
#endif

// ─── 指标与采集 ───
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/sample_exporter.hpp"
#include "neustack/metrics/metric_exporter.hpp"
#include "neustack/metrics/security_exporter.hpp"

// ─── Telemetry API ───
#include "neustack/telemetry/telemetry_api.hpp"

namespace neustack {

struct StackConfig {
    std::string local_ip = "192.168.100.2";
    uint32_t dns_server = 0x08080808;
    LogLevel log_level = LogLevel::INFO;
    bool enable_icmp = true;
    bool enable_udp = true;

    // HAL 设备选择（"tun" = 默认 TUN/TAP, "af_xdp" = AF_XDP 高性能模式）
    std::string device_type = "tun";
    std::string device_ifname = "";  // AF_XDP 用: 网卡名（空 = 默认 eth0）

    // 性能调优
    int io_cpu = -1;  // IO 线程绑核 (-1 = 不绑)

    // 防火墙配置
    bool enable_firewall = true;          // 启用防火墙
    bool firewall_shadow_mode = true;     // Shadow Mode: AI 只告警不阻断

    // AI 智能面配置（留空路径 = 不启用该模型）
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;
    std::string security_model_path;      // 防火墙安全异常检测模型

    // 防火墙 AI 配置
    float security_threshold = 0.0f;      // 安全模型异常阈值（0 = 从模型 metadata 读取）

    // 数据采集输出目录（空 = 不采集）
    std::string data_output_dir;

    // 安全数据标注 (仅采集时生效)
    int security_label = 0;           // 0=正常, 1=异常

    // TLS/HTTPS 配置（需要 NEUSTACK_TLS_ENABLED）
    std::string tls_cert_path;        // PEM 证书文件路径
    std::string tls_key_path;         // PEM 私钥文件路径
    std::string tls_ca_path;          // CA 证书路径（客户端验证用，空 = 跳过验证）
};

class NeuStack {
public:
    static std::unique_ptr<NeuStack> create(const StackConfig &config = {});
    ~NeuStack();

    // ─── HAL 层 ───
    NetDevice &device();

    // ─── 防火墙（通过门面访问）───
    bool firewall_enabled() const;
    bool firewall_ai_enabled() const;
    void firewall_set_shadow_mode(bool shadow);
    bool firewall_shadow_mode() const;
    void firewall_set_threshold(float threshold);

    /// 防火墙包检查（用于手动事件循环）
    /// @return true = 放行, false = 丢弃
    bool firewall_inspect(const uint8_t* data, size_t len);

    /// 防火墙定时器（手动事件循环时每秒调用一次）
    void firewall_on_timer();

    /// 防火墙规则引擎（高级用户直接操作规则）
    RuleEngine* firewall_rules();

    /// 防火墙统计
    FirewallStats firewall_stats() const;
    FirewallAIStats firewall_ai_stats() const;

    // ─── 网络层 ───
    IPv4Layer    &ip();
    ICMPHandler  *icmp();          // 未启用时返回 nullptr

    // ─── 传输层 ───
    TCPLayer  &tcp();
    UDPLayer  *udp();              // 未启用时返回 nullptr

    // ─── 应用层 ───
    HttpServer &http_server();
    HttpClient &http_client();
    DNSClient  *dns();             // 依赖 UDP，未启用时返回 nullptr

    // ─── TLS / HTTPS ───
#ifdef NEUSTACK_TLS_ENABLED
    HttpServer *https_server();    // TLS 未配置时返回 nullptr
    HttpClient *https_client();    // TLS 未配置时返回 nullptr
    TLSLayer   *tls();             // TLS 未配置时返回 nullptr
#endif

    // ─── 指标与采集 ───
    GlobalMetrics   &metrics();
    SampleExporter  *sample_exporter();    // 未配置采集时返回 nullptr
    MetricsExporter *metrics_exporter();   // 未配置采集时返回 nullptr
    SecurityExporter *security_exporter(); // 未配置采集时返回 nullptr

    // ─── Telemetry API ───
    telemetry::TelemetryAPI &telemetry();
    const telemetry::TelemetryAPI &telemetry() const;

    // 便捷方法 (等同于 telemetry().to_json() / to_prometheus())
    std::string status_json(bool pretty = false);
    std::string status_prometheus();

    // ─── AI 智能面 ───
    bool ai_enabled() const;

    // ─── 事件循环 ───
    void run();                    // 阻塞运行，Ctrl+C 退出
    void stop();                   // 从另一个线程/信号调用
    bool running() const;          // 查询运行状态

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    NeuStack(std::unique_ptr<Impl> impl);
};

} // namespace neustack

#endif // NEUSTACK_NEUSTACK_HPP
