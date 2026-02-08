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

// ─── 指标与采集 ───
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/sample_exporter.hpp"
#include "neustack/metrics/metric_exporter.hpp"

namespace neustack {

struct StackConfig {
    std::string local_ip = "192.168.100.2";
    uint32_t dns_server = 0x08080808;
    LogLevel log_level = LogLevel::INFO;
    bool enable_icmp = true;
    bool enable_udp = true;

    // AI 智能面配置（留空路径 = 不启用该模型）
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;

    // 数据采集输出目录（空 = 不采集）
    std::string data_output_dir;
};

class NeuStack {
public:
    static std::unique_ptr<NeuStack> create(const StackConfig &config = {});
    ~NeuStack();

    // ─── HAL 层 ───
    NetDevice &device();

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
    SampleExporter  *sample_exporter();   // 未配置采集时返回 nullptr
    MetricsExporter *metrics_exporter();  // 未配置采集时返回 nullptr

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
