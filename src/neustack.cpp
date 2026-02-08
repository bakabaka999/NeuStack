#include "neustack/neustack.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/ai/intelligence_plane.hpp"
#endif

#include <chrono>
#include <atomic>

namespace neustack {

struct NeuStack::Impl {
    StackConfig config;
    std::atomic<bool> running{false};

    // 核心协议层
    std::unique_ptr<NetDevice> device;
    std::unique_ptr<IPv4Layer> ip;
    std::unique_ptr<ICMPHandler> icmp;
    std::unique_ptr<UDPLayer> udp;
    std::unique_ptr<TCPLayer> tcp;

    // 应用层服务
    std::unique_ptr<HttpServer> http_server;
    std::unique_ptr<HttpClient> http_client;
    std::unique_ptr<DNSClient> dns_client;

    // 数据采集
    std::unique_ptr<SampleExporter> sample_exporter;
    std::unique_ptr<MetricsExporter> metrics_exporter;

    Impl(const StackConfig &cfg) : config(cfg) {}

    bool initialize() {
        // 1. HAL 层
        device = NetDevice::create();
        if (!device || device->open() < 0) {
            LOG_FATAL(HAL, "Failed to open device");
            return false;
        }
        LOG_INFO(HAL, "Device: %s", device->get_name().c_str());

        // 2. 网络层
        uint32_t local_ip = ip_from_string(config.local_ip.c_str());
        ip = std::make_unique<IPv4Layer>(*device);
        ip->set_local_ip(local_ip);

        if (config.enable_icmp) {
            icmp = std::make_unique<ICMPHandler>(*ip);
            ip->register_handler(static_cast<uint8_t>(IPProtocol::ICMP), icmp.get());
        }

        // 3. 传输层 (UDP)
        if (config.enable_udp) {
            udp = std::make_unique<UDPLayer>(*ip);
            ip->register_handler(static_cast<uint8_t>(IPProtocol::UDP), udp.get());
        }

        // 4. 传输层 (TCP)
        tcp = std::make_unique<TCPLayer>(*ip, local_ip);
        tcp->set_default_options(TCPOptions::high_throughput());
        ip->register_handler(static_cast<uint8_t>(IPProtocol::TCP), tcp.get());

        // 5. 应用层
        http_server = std::make_unique<HttpServer>(*tcp);
        http_client = std::make_unique<HttpClient>(*tcp);

        if (udp) {
            dns_client = std::make_unique<DNSClient>(*udp, config.dns_server);
            dns_client->init();
        }

        // 6. AI 智能面
#ifdef NEUSTACK_AI_ENABLED
        bool has_any_model = !config.orca_model_path.empty()
                          || !config.anomaly_model_path.empty()
                          || !config.bandwidth_model_path.empty();
        if (has_any_model) {
            IntelligencePlaneConfig ai_cfg;
            ai_cfg.orca_model_path = config.orca_model_path;
            ai_cfg.anomaly_model_path = config.anomaly_model_path;
            ai_cfg.bandwidth_model_path = config.bandwidth_model_path;
            tcp->enable_ai(ai_cfg);
            LOG_INFO(APP, "AI intelligence plane enabled");
        }
#endif

        // 7. 数据采集
        if (!config.data_output_dir.empty()) {
            std::string samples_path = config.data_output_dir + "/tcp_samples.csv";
            std::string metrics_path = config.data_output_dir + "/global_metrics.csv";
            sample_exporter = std::make_unique<SampleExporter>(
                samples_path, tcp->metrics_buffer());
            metrics_exporter = std::make_unique<MetricsExporter>(metrics_path);
            LOG_INFO(APP, "Data collection: %s", config.data_output_dir.c_str());
        }

        return true;
    }
};

// ─── 构造 / 析构 ───

NeuStack::NeuStack(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

NeuStack::~NeuStack() {
    if (_impl) {
        _impl->running = false;
        if (_impl->sample_exporter) _impl->sample_exporter->flush();
        if (_impl->metrics_exporter) _impl->metrics_exporter->flush();
        if (_impl->device) _impl->device->close();
    }
}

std::unique_ptr<NeuStack> NeuStack::create(const StackConfig &config) {
    auto impl = std::make_unique<Impl>(config);
    if (!impl->initialize()) {
        return nullptr;
    }
    return std::unique_ptr<NeuStack>(new NeuStack(std::move(impl)));
}

// ─── HAL 层 ───

NetDevice &NeuStack::device() { return *_impl->device; }

// ─── 网络层 ───

IPv4Layer   &NeuStack::ip()   { return *_impl->ip; }
ICMPHandler *NeuStack::icmp() { return _impl->icmp.get(); }

// ─── 传输层 ───

TCPLayer &NeuStack::tcp() { return *_impl->tcp; }
UDPLayer *NeuStack::udp() { return _impl->udp.get(); }

// ─── 应用层 ───

HttpServer &NeuStack::http_server() { return *_impl->http_server; }
HttpClient &NeuStack::http_client() { return *_impl->http_client; }
DNSClient  *NeuStack::dns()         { return _impl->dns_client.get(); }

// ─── 指标与采集 ───

GlobalMetrics   &NeuStack::metrics()          { return global_metrics(); }
SampleExporter  *NeuStack::sample_exporter()  { return _impl->sample_exporter.get(); }
MetricsExporter *NeuStack::metrics_exporter() { return _impl->metrics_exporter.get(); }

// ─── AI 智能面 ───

bool NeuStack::ai_enabled() const { return _impl->tcp->ai_enabled(); }

// ─── 事件循环 ───

void NeuStack::run() {
    _impl->running = true;
    uint8_t buf[2048];
    auto last_timer = std::chrono::steady_clock::now();
    constexpr auto TIMER_INTERVAL = std::chrono::milliseconds(100);

    LOG_INFO(APP, "NeuStack running on %s", _impl->config.local_ip.c_str());

    while (_impl->running) {
        // 1. 收包
        ssize_t n = _impl->device->recv(buf, sizeof(buf), 10);
        if (n > 0) {
            _impl->ip->on_receive(buf, n);
        }

        // 2. 应用层轮询
        _impl->http_server->poll();

        // 3. 定时器 (100ms)
        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= TIMER_INTERVAL) {
            _impl->tcp->on_timer();
            if (_impl->dns_client) _impl->dns_client->on_timer();

            // 数据采集导出
            if (_impl->sample_exporter) _impl->sample_exporter->export_new_samples();
            if (_impl->metrics_exporter) _impl->metrics_exporter->export_delta(100);

            last_timer = now;
        }
    }

    LOG_INFO(APP, "NeuStack stopped");
}

void NeuStack::stop() {
    _impl->running = false;
}

bool NeuStack::running() const {
    return _impl->running;
}

} // namespace neustack
