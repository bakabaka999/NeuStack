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
    std::unique_ptr<FirewallEngine> firewall;  // 防火墙（HAL 和 IPv4 之间）
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
    std::unique_ptr<SecurityExporter> security_exporter;
    uint32_t security_tick_count = 0;  // SecurityExporter 每秒 flush 一次

    Impl(const StackConfig &cfg) : config(cfg) {}

    bool initialize() {
        // 1. HAL 层
        device = NetDevice::create();
        if (!device || device->open() < 0) {
            LOG_FATAL(HAL, "Failed to open device");
            return false;
        }
        LOG_INFO(HAL, "Device: %s", device->get_name().c_str());

        // 2. 防火墙（HAL 和 IPv4 之间）
        if (config.enable_firewall) {
            FirewallConfig fw_cfg;
            fw_cfg.enabled = true;
            fw_cfg.shadow_mode = config.firewall_shadow_mode;
            firewall = std::make_unique<FirewallEngine>(fw_cfg);

            // 加载安全异常检测模型
            if (!config.security_model_path.empty()) {
                if (firewall->enable_ai(config.security_model_path, 
                                         config.security_threshold)) {
                    LOG_INFO(FW, "Security AI model loaded: %s", 
                             config.security_model_path.c_str());
                } else {
                    LOG_WARN(FW, "Failed to load security AI model: %s", 
                             config.security_model_path.c_str());
                }
            }
        }

        // 3. 网络层
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

            // 安全指标采集（需要防火墙启用）
            if (firewall && firewall->ai()) {
                std::string security_path = config.data_output_dir + "/security_metrics.csv";
                security_exporter = std::make_unique<SecurityExporter>(
                    security_path, firewall->ai()->metrics());
            }

            LOG_INFO(APP, "Data collection: %s", config.data_output_dir.c_str());
        }

        // 8. 安全数据采集 (有采集目录 + 防火墙启用 → 自动创建)
        if (!config.data_output_dir.empty() && firewall) {
            // 采集模式下需要 FirewallAI 的 metrics，如果 AI 尚未初始化则创建一个（不加载模型）
            if (!firewall->ai()) {
                FirewallAIConfig ai_cfg;
                ai_cfg.shadow_mode = config.firewall_shadow_mode;
                firewall->set_ai(std::make_unique<FirewallAI>(ai_cfg));
            }
            std::string sec_path = config.data_output_dir + "/security_data.csv";
            security_exporter = std::make_unique<SecurityExporter>(
                sec_path, firewall->ai()->metrics());
            LOG_INFO(APP, "Security collection: %s (label=%d)",
                     sec_path.c_str(), config.security_label);
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
        if (_impl->security_exporter) _impl->security_exporter->sync();
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

// ─── 防火墙（门面封装）───

bool NeuStack::firewall_enabled() const {
    return _impl->firewall && _impl->firewall->enabled();
}

bool NeuStack::firewall_ai_enabled() const {
    return _impl->firewall && _impl->firewall->ai_enabled();
}

void NeuStack::firewall_set_shadow_mode(bool shadow) {
    if (_impl->firewall) _impl->firewall->set_shadow_mode(shadow);
}

bool NeuStack::firewall_shadow_mode() const {
    return _impl->firewall ? _impl->firewall->shadow_mode() : true;
}

void NeuStack::firewall_set_threshold(float threshold) {
    if (_impl->firewall && _impl->firewall->ai()) {
        _impl->firewall->ai()->set_threshold(threshold);
    }
}

bool NeuStack::firewall_inspect(const uint8_t* data, size_t len) {
    if (!_impl->firewall) return true;
    return _impl->firewall->inspect(data, len);
}

void NeuStack::firewall_on_timer() {
    if (!_impl->firewall) return;
    _impl->firewall->on_timer();
    if (_impl->security_exporter) {
        _impl->security_exporter->flush();
    }
}

RuleEngine* NeuStack::firewall_rules() {
    return _impl->firewall ? &_impl->firewall->rule_engine() : nullptr;
}

FirewallStats NeuStack::firewall_stats() const {
    return _impl->firewall ? _impl->firewall->stats() : FirewallStats{};
}

FirewallAIStats NeuStack::firewall_ai_stats() const {
    return (_impl->firewall && _impl->firewall->ai()) 
        ? _impl->firewall->ai()->stats() : FirewallAIStats{};
}

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
SecurityExporter *NeuStack::security_exporter() { return _impl->security_exporter.get(); }

// ─── AI 智能面 ───

bool NeuStack::ai_enabled() const { return _impl->tcp->ai_enabled(); }

// ─── 事件循环 ───

void NeuStack::run() {
    _impl->running = true;
    uint8_t buf[2048];
    auto last_timer = std::chrono::steady_clock::now();
    auto last_fw_timer = std::chrono::steady_clock::now();
    constexpr auto TIMER_INTERVAL = std::chrono::milliseconds(100);
    constexpr auto FW_TIMER_INTERVAL = std::chrono::seconds(1);

    LOG_INFO(APP, "NeuStack running on %s", _impl->config.local_ip.c_str());

    while (_impl->running) {
        // 1. 收包
        ssize_t n = _impl->device->recv(buf, sizeof(buf), 10);
        if (n > 0) {
            // 2. 防火墙检查
            bool pass = true;
            if (_impl->firewall) {
                pass = _impl->firewall->inspect(buf, static_cast<size_t>(n));
            }

            // 3. 放行则交给 IPv4 层处理
            if (pass) {
                _impl->ip->on_receive(buf, n);
            }
        }

        // 4. 应用层轮询
        _impl->http_server->poll();

        // 5. 定时器 (100ms)
        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= TIMER_INTERVAL) {
            _impl->tcp->on_timer();
            if (_impl->dns_client) _impl->dns_client->on_timer();
            if (_impl->firewall) _impl->firewall->on_timer();

            // 数据采集导出
            if (_impl->sample_exporter) _impl->sample_exporter->export_new_samples();
            if (_impl->metrics_exporter) _impl->metrics_exporter->export_delta(100);

            // 安全数据导出 (每秒 flush 一次 = 每 10 个 100ms tick)
            if (_impl->security_exporter) {
                _impl->security_tick_count++;
                if (_impl->security_tick_count >= 10) {
                    _impl->security_tick_count = 0;
                    _impl->security_exporter->flush(_impl->config.security_label);
                }
            }

            last_timer = now;
        }

        // 6. 防火墙定时器 (1s) — tick + AI 推理 + 安全数据采集
        if (_impl->firewall && now - last_fw_timer >= FW_TIMER_INTERVAL) {
            _impl->firewall->on_timer();

            if (_impl->security_exporter) {
                _impl->security_exporter->flush();
            }

            last_fw_timer = now;
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
