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

// ─── 指标与采集 ───
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/sample_exporter.hpp"
#include "neustack/metrics/metric_exporter.hpp"
#include "neustack/metrics/security_exporter.hpp"

namespace neustack {

struct StackConfig {
    std::string local_ip = "192.168.100.2";
    uint32_t dns_server = 0x08080808;
    LogLevel log_level = LogLevel::INFO;
    bool enable_icmp = true;
    bool enable_udp = true;

    // 防火墙配置
    bool enable_firewall = true;          // 启用防火墙
    bool firewall_shadow_mode = true;     // Shadow Mode: AI 只告警不阻断

    // AI 智能面配置（留空路径 = 不启用该模型）
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;
    std::string security_model_path;      // 防火墙安全异常检测模型

    // 防火墙 AI 配置
    float security_threshold = 0.5f;      // 安全模型异常阈值

    // 数据采集输出目录（空 = 不采集）
    std::string data_output_dir;
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

    // ─── 指标与采集 ───
    GlobalMetrics   &metrics();
    SampleExporter  *sample_exporter();    // 未配置采集时返回 nullptr
    MetricsExporter *metrics_exporter();   // 未配置采集时返回 nullptr
    SecurityExporter *security_exporter(); // 未配置采集时返回 nullptr

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
