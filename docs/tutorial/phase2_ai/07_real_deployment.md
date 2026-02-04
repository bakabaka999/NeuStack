# 教程 07：真实网卡部署与真实数据采集

> **前置要求**: 完成教程 01-06，NeuStack 编译通过，ONNX 模型已可加载
> **目标**: 在真实网络环境中运行 NeuStack，采集 TCPSample 数据，替换模拟训练

## 1. 为什么需要真实数据？

教程 06 中我们用 `training/orca/env.py` 的 `SimpleNetworkEnv` 训练了 Orca 模型。
这是一个纯数学模拟器——带宽用正弦波变化，丢包率用阈值函数，排队延迟用简单公式：

```python
# env.py 的模拟逻辑
queue_growth = excess_rate * 0.01
loss_rate = 0.1 + 0.4 * (queue_size / buffer_size - 0.9) / 0.1
throughput = min(send_rate, current_bw) * (1 - loss_rate)
```

真实网络中，这些都远比数学公式复杂：

| 现象 | 模拟环境 | 真实网络 |
|------|----------|----------|
| 排队延迟 | 线性增长 | AQM (CoDel, PIE) 主动丢包 |
| 丢包模式 | 均匀随机 | 突发丢包、尾部丢包 |
| 带宽波动 | 平滑正弦 | 突变（WiFi 切换、链路聚合） |
| 竞争流量 | 简单分配 | 真实 CUBIC/BBR 行为 |
| ACK 时序 | 理想间隔 | ACK 压缩、延迟 ACK |

**核心问题**: 模拟器训练的模型在真实网络上可能完全失效。

本教程的目标：

```
┌──────────────────────────────────────────────────────────────┐
│                                                               │
│  模拟训练 (Phase 1):    env.py  ──→  train.py  ──→  model.onnx  │
│                          ↑                                    │
│                       过于简化                                 │
│                                                               │
│  真实数据 (本教程):                                             │
│                                                               │
│  NeuStack ──→ TCPSample CSV ──→ 离线训练 ──→  model.onnx     │
│    ↑                                            ↓             │
│  真实网卡                                    部署回 NeuStack    │
│  真实流量                                                     │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

## 2. 部署拓扑

### 2.1 基本拓扑

NeuStack 是**用户态协议栈**，通过虚拟网卡（TUN/utun）接收 IP 包。
要让 NeuStack 处理真实网络流量，需要把真实网卡上的流量**路由**到虚拟网卡：

```
┌─────────────────────────────────────────────────────────────┐
│  NeuStack 主机                                               │
│                                                              │
│  ┌──────────┐         ┌──────────┐         ┌──────────┐    │
│  │ 真实网卡  │ ──路由──▶│ 虚拟网卡  │ ◀──────▶│ NeuStack │    │
│  │ eth0/en0 │         │ tun0/utun│ read/write│ 用户态   │    │
│  │ 10.0.0.2 │         │ 10.0.1.2 │         │ TCP/IP   │    │
│  └──────────┘         └──────────┘         └──────────┘    │
│       ↑                                          │          │
│       │                                    TCPSample        │
│       │                                          ↓          │
│  真实网络                                  CSV / 训练数据     │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 两种部署方式

**方式 A：本地回环测试**（最简单，适合开发）

```
┌─────────────────────────────────────────┐
│  同一台机器                               │
│                                          │
│  Terminal 1:  NeuStack (utun3/tun0)      │
│               IP: 192.168.100.2          │
│                                          │
│  Terminal 2:  iperf3 -c 192.168.100.2    │
│               curl http://192.168.100.2  │
│                                          │
│  路由: 192.168.100.0/24 → utun3/tun0    │
└─────────────────────────────────────────┘
```

**方式 B：跨主机测试**（接近生产环境）

```
┌──────────────┐                    ┌──────────────┐
│  客户端机器    │                    │ NeuStack 主机 │
│              │                    │              │
│  iperf3 -c   │──── 真实网络 ────▶│  eth0 (转发)  │
│  10.0.1.2    │  (WiFi/有线/WAN)  │       ↓       │
│              │                    │  tun0 (NeuS.) │
│              │                    │  10.0.1.2     │
└──────────────┘                    └──────────────┘
```

## 3. macOS 部署（utun 已实现）

macOS 上 NeuStack 已通过 utun 完全可用。

### 3.1 启动 NeuStack

```bash
# 编译
cd build && cmake .. -DNEUSTACK_ENABLE_AI=ON && make -j

# 启动（需要 root 权限创建 utun 设备）
sudo ./neustack --ip 192.168.100.2 -v
```

启动后会打印设备名称，例如：

```
[INFO ] HAL : device: utun3
```

### 3.2 配置虚拟网卡

在另一个终端：

```bash
# 给 utun 接口配置 IP 地址
# 192.168.100.1 = 主机侧 IP (内核协议栈)
# 192.168.100.2 = NeuStack 侧 IP (用户态)
sudo ifconfig utun3 192.168.100.1 192.168.100.2 up

# 验证
ping 192.168.100.2
# 应该能收到 ICMP 回复 (NeuStack 的 ICMPHandler)
```

### 3.3 utun 工作原理回顾

```cpp
// src/hal/hal_macos.cpp

int MacOSDevice::open() {
    // 1. 创建系统控制 socket
    _fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);

    // 2. 获取 utun 驱动的控制 ID
    struct ctl_info info{};
    std::strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));
    ioctl(_fd, CTLIOCGINFO, &info);

    // 3. 连接 → 内核自动创建 utunN 设备
    struct sockaddr_ctl sc{};
    sc.sc_id = info.ctl_id;
    sc.sc_unit = 0;  // 系统自动分配编号
    connect(_fd, (struct sockaddr*)&sc, sizeof(sc));

    // 4. 读取分配的设备名
    getsockopt(_fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &ifname_len);
}
```

**关键点**: macOS utun 是 L3 设备（无以太网头），收发的是**裸 IP 包**，
但每个包前面有 4 字节协议族前缀（`AF_INET = htonl(2)`）。

```cpp
// 发送: 在 IP 包前加 4 字节 AF_INET
ssize_t MacOSDevice::send(const uint8_t* data, size_t len) {
    uint32_t proto = htonl(AF_INET);
    memcpy(buf, &proto, 4);
    memcpy(buf + 4, data, len);      // data = 完整 IP 包
    write(_fd, buf, 4 + len);
}

// 接收: 跳过前 4 字节
ssize_t MacOSDevice::recv(uint8_t* buf, size_t len, int timeout_ms) {
    ssize_t n = read(_fd, recv_buf, sizeof(recv_buf));
    memcpy(buf, recv_buf + 4, n - 4); // 返回裸 IP 包
}
```

## 4. Linux 部署（TUN 设备）

Linux 上使用 TUN 设备，与 macOS utun 同属 L3 隧道。

### 4.1 TUN 设备原理

```
┌──────────────────────────────────────────────────────┐
│  Linux 内核                                           │
│                                                       │
│  用户空间程序:   NeuStack                              │
│       ↑ read()         ↓ write()                      │
│  ┌────┴────────────────┴────┐                         │
│  │  /dev/net/tun (字符设备)   │                         │
│  │  IFF_TUN | IFF_NO_PI     │                         │
│  └────┬────────────────┬────┘                         │
│       ↓                ↑                              │
│  ┌────┴────────────────┴────┐                         │
│  │  tun0 (虚拟网络接口)       │                         │
│  │  与 eth0 等普通接口无异     │                         │
│  │  可配置 IP、路由、iptables  │                         │
│  └──────────────────────────┘                         │
└──────────────────────────────────────────────────────┘
```

### 4.2 LinuxDevice 实现

当前 `hal_linux.hpp` 只有声明，需要实现 `hal_linux.cpp`：

```cpp
// src/hal/hal_linux.cpp

#ifdef NEUSTACK_PLATFORM_LINUX

#include "neustack/hal/hal_linux.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace neustack;

int LinuxDevice::open() {
    // 1. 打开 TUN 字符设备
    _fd = ::open("/dev/net/tun", O_RDWR);
    if (_fd < 0) {
        LOG_ERROR(HAL, "open(/dev/net/tun) failed: %s", std::strerror(errno));
        return -1;
    }

    // 2. 配置为 TUN 模式 (L3, 无协议信息前缀)
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    // IFF_TUN    = L3 隧道 (IP 包, 无以太网头)
    // IFF_NO_PI  = 不加协议信息前缀 (与 macOS utun 不同!)

    if (ioctl(_fd, TUNSETIFF, &ifr) < 0) {
        LOG_ERROR(HAL, "ioctl(TUNSETIFF) failed: %s", std::strerror(errno));
        ::close(_fd);
        _fd = -1;
        return -1;
    }

    _name = ifr.ifr_name;  // 内核分配的名称, 如 "tun0"

    // 3. 设置非阻塞
    int flags = fcntl(_fd, F_GETFL, 0);
    fcntl(_fd, F_SETFL, flags | O_NONBLOCK);

    LOG_DEBUG(HAL, "TUN device opened: %s (fd=%d)", _name.c_str(), _fd);
    return 0;
}

int LinuxDevice::close() {
    if (_fd >= 0) {
        LOG_DEBUG(HAL, "closing device %s", _name.c_str());
        ::close(_fd);
        _fd = -1;
        _name.clear();
    }
    return 0;
}

ssize_t LinuxDevice::send(const uint8_t* data, size_t len) {
    if (_fd < 0) return -1;

    // Linux TUN + IFF_NO_PI: 直接写裸 IP 包, 无前缀
    ssize_t n = write(_fd, data, len);
    if (n < 0) {
        LOG_ERROR(HAL, "write failed: %s", std::strerror(errno));
        return -1;
    }

    LOG_TRACE(HAL, "sent %zd bytes", n);
    return n;
}

ssize_t LinuxDevice::recv(uint8_t* buf, size_t len, int timeout_ms) {
    if (_fd < 0) return -1;

    // 使用 poll 实现超时
    if (timeout_ms >= 0) {
        struct pollfd pfd{};
        pfd.fd = _fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) return 0;
            LOG_ERROR(HAL, "poll failed: %s", std::strerror(errno));
            return -1;
        }
        if (ret == 0) return 0;  // 超时
    }

    // Linux TUN + IFF_NO_PI: 直接读裸 IP 包, 无前缀
    ssize_t n = read(_fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_ERROR(HAL, "read failed: %s", std::strerror(errno));
        return -1;
    }

    LOG_TRACE(HAL, "received %zd bytes", n);
    return n;
}

int LinuxDevice::get_fd() const { return _fd; }
std::string LinuxDevice::get_name() const { return _name; }

#endif // NEUSTACK_PLATFORM_LINUX
```

> **macOS vs Linux 的关键差异**:
>
> | | macOS utun | Linux TUN |
> |---|---|---|
> | 创建方式 | PF_SYSTEM socket | `open("/dev/net/tun")` |
> | 包格式 | 4 字节 AF_INET 前缀 + IP 包 | 裸 IP 包 (IFF_NO_PI) |
> | 设备命名 | `utunN` (系统分配) | `tunN` (系统分配) |
> | 配置方式 | `ifconfig` | `ip addr/link` |

### 4.3 Linux 部署步骤

```bash
# 1. 编译 (确保在 Linux 上)
cd build && cmake .. -DNEUSTACK_ENABLE_AI=ON && make -j

# 2. 确保 /dev/net/tun 存在
ls -la /dev/net/tun
# 如果不存在: sudo modprobe tun

# 3. 启动 NeuStack (需要 root 或 CAP_NET_ADMIN)
sudo ./neustack --ip 10.0.1.2 -v
# 输出: [INFO ] HAL : device: tun0

# 4. 配置 TUN 接口 (另一个终端)
sudo ip addr add 10.0.1.1/24 dev tun0
sudo ip link set tun0 up

# 5. 验证
ping 10.0.1.2
```

### 4.4 CMake 条件编译

Linux TUN 实现需要加入构建系统：

```cmake
# CMakeLists.txt (HAL 源文件部分)

if(NEUSTACK_PLATFORM_MACOS)
    list(APPEND HAL_SOURCES src/hal/hal_macos.cpp)
elseif(NEUSTACK_PLATFORM_LINUX)
    list(APPEND HAL_SOURCES src/hal/hal_linux.cpp)
endif()
```

## 5. 路由与转发配置

本地回环（方式 A）配置完虚拟网卡 IP 即可直接使用。
跨主机（方式 B）需要额外的路由和转发配置。

### 5.1 方式 B: Linux IP 转发

将到达 NeuStack IP 的流量从真实网卡转发到 TUN 设备：

```bash
# 在 NeuStack 主机上:

# 1. 启用 IP 转发
sudo sysctl -w net.ipv4.ip_forward=1

# 2. 配置 NeuStack 的 TUN IP
sudo ip addr add 10.0.1.1/24 dev tun0
sudo ip link set tun0 up

# 3. 添加 DNAT 规则: 发给本机特定端口的包 → 转发到 NeuStack
#    假设 eth0 IP = 10.0.0.2, NeuStack IP = 10.0.1.2
sudo iptables -t nat -A PREROUTING -i eth0 -p tcp --dport 80 \
    -j DNAT --to-destination 10.0.1.2:80

# 4. 允许转发
sudo iptables -A FORWARD -i eth0 -o tun0 -j ACCEPT
sudo iptables -A FORWARD -i tun0 -o eth0 -j ACCEPT

# 5. MASQUERADE 回包
sudo iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
```

```
客户端 10.0.0.100                NeuStack 主机 10.0.0.2
   │                                   │
   │ SYN → 10.0.0.2:80                 │
   │──────────────────────────────────▶│
   │                           iptables DNAT
   │                           → 10.0.1.2:80
   │                                   │
   │                              ┌────▼────┐
   │                              │  tun0   │
   │                              │ NeuStack│
   │                              └────┬────┘
   │                                   │
   │ ◀──── SYN-ACK ──────────────────│
```

### 5.2 方式 B: macOS IP 转发

macOS 使用 `pfctl` 实现类似功能：

```bash
# 1. 启用 IP 转发
sudo sysctl -w net.inet.ip.forwarding=1

# 2. 配置 PF 规则
cat > /tmp/neustack_pf.conf << 'EOF'
# 将 en0:80 的流量重定向到 NeuStack
rdr on en0 proto tcp from any to (en0) port 80 -> 192.168.100.2 port 80
# 允许转发
pass all
EOF

sudo pfctl -f /tmp/neustack_pf.conf
sudo pfctl -e
```

## 6. 生成真实流量

虚拟网卡配置好后，需要向 NeuStack 发送真实 TCP 流量来采集数据。

### 6.1 流量生成工具

```bash
# ─── iperf3: 高吞吐量 TCP 流量 (最推荐) ───

# NeuStack 已经有 TCP Echo 服务 (port 7)
# 但更好的方式是用 NeuStack 的 HTTP 服务 + curl/ab

# ─── curl: HTTP 请求 ───
curl http://192.168.100.2/
curl http://192.168.100.2/api/status

# ─── Apache Bench: 并发压力测试 ───
ab -n 1000 -c 10 http://192.168.100.2/api/status

# ─── wrk: 高性能 HTTP 压测 ───
wrk -t4 -c100 -d30s http://192.168.100.2/

# ─── ncat: 原始 TCP 流量 ───
# 向 NeuStack TCP Echo 端口发送数据
echo "Hello NeuStack" | ncat 192.168.100.2 7

# ─── 大文件传输 ───
dd if=/dev/urandom bs=1M count=100 | ncat 192.168.100.2 7 > /dev/null
```

### 6.2 多场景数据采集

训练好的模型需要见过各种网络条件。建议采集以下场景：

| 场景 | 工具 | 目的 |
|------|------|------|
| 空闲基线 | `ping -c 100` | 测量 min_RTT 基线 |
| 稳定吞吐 | `wrk -c1 -d60s` | 单连接稳定状态 |
| 并发竞争 | `wrk -c50 -d60s` | 多连接竞争带宽 |
| 突发流量 | `ab -n 5000 -c 100` | 短连接突发 |
| 大文件 | `dd + ncat` | 长连接持续传输 |
| 丢包环境 | `tc netem` | 人为注入丢包和延迟 |

### 6.3 用 tc netem 模拟网络条件

在 Linux 上可以用 `tc`（Traffic Control）给 TUN 设备注入真实的网络缺陷：

```bash
# 添加 50ms 延迟 + 10ms 抖动
sudo tc qdisc add dev tun0 root netem delay 50ms 10ms

# 添加 2% 随机丢包
sudo tc qdisc change dev tun0 root netem delay 50ms 10ms loss 2%

# 限制带宽为 10Mbps
sudo tc qdisc add dev tun0 root tbf rate 10mbit burst 32kbit latency 50ms

# 组合: 模拟 4G 网络
sudo tc qdisc add dev tun0 root netem \
    delay 30ms 15ms distribution normal \
    loss 1% 25% \
    rate 20mbit

# 清除所有规则
sudo tc qdisc del dev tun0 root

# macOS 上使用 dnctl (简化版)
sudo dnctl pipe 1 config bw 10Mbit/s delay 50ms plr 0.02
sudo pfctl -e
echo "dummynet-anchor neustack" | sudo pfctl -f -
```

## 7. TCPSample 数据导出

### 7.1 现有的 MetricsExporter

NeuStack 已有 `MetricsExporter` 用于导出 `GlobalMetrics` 快照到 CSV：

```cpp
// include/neustack/metrics/metric_exporter.hpp

class MetricsExporter {
public:
    explicit MetricsExporter(const std::string& filepath);

    // 导出 GlobalMetrics 快照差值
    void export_delta(uint64_t interval_ms);
};
```

但 GlobalMetrics 只有全局计数器（packets_rx, syn_received 等），
**缺少 Per-Connection 的 TCPSample 导出**。

### 7.2 新增 SampleExporter

需要一个新类来把 `MetricsBuffer<TCPSample>` 中的数据写入 CSV：

```cpp
// include/neustack/metrics/sample_exporter.hpp

#ifndef NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP
#define NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP

#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/common/ring_buffer.hpp"
#include <fstream>
#include <string>

namespace neustack {

/**
 * TCPSample CSV 导出器
 *
 * 定期从 MetricsBuffer 读取新样本，追加写入 CSV 文件。
 * 用于离线训练数据采集。
 */
class SampleExporter {
public:
    explicit SampleExporter(
        const std::string& filepath,
        MetricsBuffer<TCPSample, 1024>& metrics_buf
    )
        : _file(filepath)
        , _metrics_buf(metrics_buf)
        , _last_total(0)
    {
        // CSV 头
        _file << "timestamp_us,"
              << "rtt_us,min_rtt_us,srtt_us,"
              << "cwnd,ssthresh,bytes_in_flight,"
              << "delivery_rate,send_rate,"
              << "loss_detected,timeout_occurred,ecn_ce_count,"
              << "is_app_limited,packets_sent,packets_lost\n";
    }

    /**
     * 导出新增的样本
     *
     * 通过比较 total_pushed() 检测是否有新数据。
     * 典型调用频率: 每 100ms 或每 1s。
     */
    void export_new_samples() {
        size_t current_total = _metrics_buf.total_pushed();
        if (current_total == _last_total) return;  // 无新数据

        // 读取最近的样本
        size_t new_count = current_total - _last_total;
        auto samples = _metrics_buf.recent(
            std::min(new_count, static_cast<size_t>(1024))
        );

        for (const auto& s : samples) {
            // 跳过已经导出过的 (按时间戳去重)
            if (s.timestamp_us <= _last_timestamp) continue;

            _file << s.timestamp_us << ","
                  << s.rtt_us << "," << s.min_rtt_us << "," << s.srtt_us << ","
                  << s.cwnd << "," << s.ssthresh << "," << s.bytes_in_flight << ","
                  << s.delivery_rate << "," << s.send_rate << ","
                  << static_cast<int>(s.loss_detected) << ","
                  << static_cast<int>(s.timeout_occurred) << ","
                  << static_cast<int>(s.ecn_ce_count) << ","
                  << static_cast<int>(s.is_app_limited) << ","
                  << s.packets_sent << "," << s.packets_lost << "\n";

            _last_timestamp = s.timestamp_us;
            _exported_count++;
        }

        _last_total = current_total;
    }

    void flush() { _file.flush(); }
    size_t exported_count() const { return _exported_count; }

private:
    std::ofstream _file;
    MetricsBuffer<TCPSample, 1024>& _metrics_buf;
    size_t _last_total;
    uint64_t _last_timestamp = 0;
    size_t _exported_count = 0;
};

} // namespace neustack

#endif // NEUSTACK_METRICS_SAMPLE_EXPORTER_HPP
```

### 7.3 集成到主程序

在 main.cpp 的事件循环中加入周期性导出：

```cpp
// src/main.cpp (修改后)

#include "neustack/metrics/sample_exporter.hpp"
#include "neustack/metrics/metric_exporter.hpp"

// 在 main() 中, TCPLayer 创建之后:

// ─── 数据采集 (可选) ───
std::unique_ptr<SampleExporter> sample_exporter;
std::unique_ptr<MetricsExporter> metrics_exporter;

bool collect_data = /* 从命令行参数读取 --collect */;
if (collect_data) {
    sample_exporter = std::make_unique<SampleExporter>(
        "tcp_samples.csv",
        tcp.metrics_buffer()  // 需要 TCPLayer 暴露 MetricsBuffer 引用
    );
    metrics_exporter = std::make_unique<MetricsExporter>(
        "global_metrics.csv"
    );
    LOG_INFO(APP, "data collection enabled: tcp_samples.csv, global_metrics.csv");
}

// 在 run_event_loop() 中, 定时器回调处:

auto now = std::chrono::steady_clock::now();
if (now - last_timer >= TIMER_INTERVAL) {
    tcp_layer.on_timer();
    dns.on_timer();

    // 数据导出 (每 100ms)
    if (sample_exporter) {
        sample_exporter->export_new_samples();
    }
    if (metrics_exporter) {
        metrics_exporter->export_delta(100);
    }

    last_timer = now;
}
```

### 7.4 命令行参数扩展

```cpp
// Config 结构体扩展
struct Config {
    // ... 现有字段 ...
    bool collect_data = false;
    std::string output_dir = ".";
};

// 解析
else if (std::strcmp(argv[i], "--collect") == 0) {
    cfg.collect_data = true;
} else if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
    cfg.output_dir = argv[++i];
}
```

## 8. 采集流程

### 8.1 完整的采集会话

```bash
# ─── 步骤 1: 启动 NeuStack (开启数据采集) ───
cd build
sudo ./neustack --ip 192.168.100.2 --collect -v

# 另一个终端:
sudo ifconfig utun3 192.168.100.1 192.168.100.2 up

# ─── 步骤 2: 生成流量 ───

# 场景 A: 稳定流量 (60 秒)
wrk -t2 -c10 -d60s http://192.168.100.2/api/status

# 场景 B: 突发流量 (1000 请求, 50 并发)
ab -n 1000 -c 50 http://192.168.100.2/api/status

# 场景 C: 大文件传输
dd if=/dev/urandom bs=1M count=50 | ncat 192.168.100.2 7 > /dev/null

# ─── 步骤 3: 停止 NeuStack (Ctrl+C) ───
# 输出文件:
#   tcp_samples.csv      - TCPSample 数据
#   global_metrics.csv   - GlobalMetrics 数据
```

### 8.2 采集数据示例

`tcp_samples.csv` 的内容：

```csv
timestamp_us,rtt_us,min_rtt_us,srtt_us,cwnd,ssthresh,bytes_in_flight,delivery_rate,send_rate,loss_detected,timeout_occurred,ecn_ce_count,is_app_limited,packets_sent,packets_lost
1706000000000,245,220,230,10,65535,14600,50000000,52000000,0,0,0,0,150,0
1706000010000,260,220,235,15,65535,21900,55000000,58000000,0,0,0,0,200,0
1706000020000,310,220,248,20,65535,29200,48000000,52000000,0,0,0,0,250,1
...
```

### 8.3 数据质量检查

```bash
# 统计样本数
wc -l tcp_samples.csv

# 检查时间跨度
head -2 tcp_samples.csv | tail -1 | cut -d, -f1  # 起始时间
tail -1 tcp_samples.csv | cut -d, -f1             # 结束时间

# 检查丢包样本数
awk -F, 'NR>1 && $10>0' tcp_samples.csv | wc -l

# 基本统计
python3 -c "
import csv
import numpy as np

data = []
with open('tcp_samples.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        data.append(row)

rtts = [int(r['rtt_us']) for r in data]
rates = [int(r['delivery_rate']) for r in data]

print(f'Samples:  {len(data)}')
print(f'RTT:      min={min(rtts)}us  avg={np.mean(rtts):.0f}us  max={max(rtts)}us')
print(f'Rate:     min={min(rates)/1e6:.1f}MB/s  avg={np.mean(rates)/1e6:.1f}MB/s')
print(f'Loss:     {sum(1 for r in data if int(r[\"loss_detected\"])>0)}/{len(data)}')
"
```

## 9. 从 CSV 到训练数据集（三个模型）

NeuStack 的三个 AI 模型使用两个 CSV 数据源：

| 模型 | 数据源 | 特征维度 | 训练方式 |
|------|--------|----------|----------|
| **Orca** (拥塞控制) | tcp_samples.csv | 6D (OrcaFeatures) | DDPG (离线) |
| **异常检测** | global_metrics.csv | 5D (AnomalyFeatures) | Autoencoder (无监督) |
| **带宽预测** | tcp_samples.csv | N×3 时序 (BandwidthFeatures) | LSTM (监督) |

```
┌────────────────────────────────────────────────────────────────┐
│  CSV 数据源                    训练数据集                        │
│                                                                 │
│  tcp_samples.csv  ────┬────▶  orca_dataset.npz      (Orca)     │
│                       │                                         │
│                       └────▶  bandwidth_dataset.npz (BW Pred)  │
│                                                                 │
│  global_metrics.csv ───────▶  anomaly_dataset.npz   (Anomaly)  │
└────────────────────────────────────────────────────────────────┘
```

### 9.1 统一数据转换脚本

```python
#!/usr/bin/env python3
"""
scripts/csv_to_dataset.py

将 NeuStack 采集的 CSV 转换为三个 AI 模型的训练数据集。

用法:
    # 生成所有模型的数据集
    python scripts/csv_to_dataset.py \
        --tcp-samples tcp_samples.csv \
        --global-metrics global_metrics.csv \
        --output-dir training/real_data/

    # 只生成 Orca 数据集
    python scripts/csv_to_dataset.py \
        --tcp-samples tcp_samples.csv \
        --model orca

输出:
    training/real_data/
    ├── orca_dataset.npz         # Orca DDPG 训练数据
    ├── anomaly_dataset.npz      # 异常检测 Autoencoder 训练数据
    └── bandwidth_dataset.npz    # 带宽预测 LSTM 训练数据
"""

import argparse
import csv
import os
import numpy as np
from typing import List, Dict, Tuple


# ============================================================================
# 常量
# ============================================================================

MSS = 1460
MAX_BW = 100e6  # 100 MB/s (归一化用)
BANDWIDTH_HISTORY_LEN = 10  # 带宽预测的历史窗口


# ============================================================================
# 通用工具函数
# ============================================================================

def load_tcp_samples(csv_path: str) -> List[Dict]:
    """加载 tcp_samples.csv"""
    samples = []
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            samples.append({k: float(v) for k, v in row.items()})
    print(f"Loaded {len(samples)} TCP samples from {csv_path}")
    return samples


def load_global_metrics(csv_path: str) -> List[Dict]:
    """加载 global_metrics.csv"""
    samples = []
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            samples.append({k: float(v) for k, v in row.items()})
    print(f"Loaded {len(samples)} global metrics from {csv_path}")
    return samples


def estimate_bandwidth(samples: List[Dict], window: int = 20) -> List[float]:
    """估计每个时刻的可用带宽"""
    est_bw_list = []
    for i, s in enumerate(samples):
        start = max(0, i - window)
        valid_rates = [
            samples[j]['delivery_rate']
            for j in range(start, i + 1)
            if not samples[j].get('is_app_limited', 0)
        ]
        est_bw = max(valid_rates) if valid_rates else s['delivery_rate']
        est_bw_list.append(max(est_bw, 1))
    return est_bw_list


# ============================================================================
# Orca 数据集生成 (DDPG: state, action, reward, next_state, done)
# ============================================================================

def compute_orca_features(sample: Dict, est_bw: float) -> np.ndarray:
    """
    从 TCPSample 提取 6 维 OrcaFeatures

    与 C++ ai_features.hpp 中的 OrcaFeatures::from_sample() 保持一致
    """
    rtt = sample['rtt_us']
    min_rtt = max(sample['min_rtt_us'], 1)
    delivery_rate = sample['delivery_rate']
    cwnd = max(sample['cwnd'], 1)
    packets_sent = max(sample['packets_sent'], 1)

    # BDP (Bandwidth-Delay Product) in MSS units
    bdp = (est_bw * min_rtt / 1e6) / MSS if min_rtt > 0 else 1

    return np.array([
        delivery_rate / max(est_bw, 1),                    # throughput_normalized
        (rtt - min_rtt) / max(min_rtt, 1),                 # queuing_delay_normalized
        rtt / max(min_rtt, 1),                              # rtt_ratio
        sample['packets_lost'] / packets_sent,             # loss_rate
        cwnd / max(bdp, 1),                                 # cwnd_normalized
        sample['bytes_in_flight'] / max(cwnd * MSS, 1),    # in_flight_ratio
    ], dtype=np.float32)


def compute_orca_reward(sample: Dict, est_bw: float) -> float:
    """Orca 奖励函数: 吞吐 - 延迟惩罚 - 丢包惩罚"""
    min_rtt = max(sample['min_rtt_us'], 1)
    rtt = sample['rtt_us']
    queuing_delay = max(0, rtt - min_rtt)

    throughput_reward = sample['delivery_rate'] / max(est_bw, 1)
    delay_penalty = 2.0 * (queuing_delay / min_rtt)
    loss_rate = sample['packets_lost'] / max(sample['packets_sent'], 1)
    loss_penalty = 10.0 * loss_rate

    return throughput_reward - delay_penalty - loss_penalty


def generate_orca_dataset(samples: List[Dict], est_bw_list: List[float]) -> Dict:
    """生成 Orca DDPG 训练数据"""
    states, actions, rewards, next_states, dones = [], [], [], [], []

    for i in range(len(samples) - 1):
        s = samples[i]
        s_next = samples[i + 1]
        est_bw = est_bw_list[i]

        state = compute_orca_features(s, est_bw)
        next_state = compute_orca_features(s_next, est_bw_list[i + 1])
        reward = compute_orca_reward(s, est_bw)

        # 从 cwnd 变化推断 alpha: cwnd_new = 2^alpha * cwnd_old
        cwnd_old = max(s['cwnd'], 1)
        cwnd_new = max(s_next['cwnd'], 1)
        alpha = np.log2(cwnd_new / cwnd_old)
        alpha = np.clip(alpha, -1, 1)

        # Episode 边界: 时间间隔 > 5秒 视为新连接
        time_gap = s_next['timestamp_us'] - s['timestamp_us']
        done = time_gap > 5_000_000

        states.append(state)
        actions.append([alpha])
        rewards.append([reward])
        next_states.append(next_state)
        dones.append([float(done)])

    return {
        'states': np.array(states, dtype=np.float32),
        'actions': np.array(actions, dtype=np.float32),
        'rewards': np.array(rewards, dtype=np.float32),
        'next_states': np.array(next_states, dtype=np.float32),
        'dones': np.array(dones, dtype=np.float32),
    }


# ============================================================================
# 异常检测数据集生成 (Autoencoder: 只需要输入, 无标签)
# ============================================================================

def compute_anomaly_features(
    current: Dict,
    prev: Dict,
    interval_sec: float = 1.0
) -> np.ndarray:
    """
    从 GlobalMetrics delta 提取 5 维 AnomalyFeatures

    与 C++ ai_features.hpp 中的 AnomalyFeatures::from_delta() 保持一致
    """
    # 计算 delta
    delta_syn = current.get('syn_received', 0) - prev.get('syn_received', 0)
    delta_rst = current.get('rst_received', 0) - prev.get('rst_received', 0)
    delta_conn = current.get('conn_established', 0) - prev.get('conn_established', 0)
    delta_packets = current.get('packets_rx', 0) - prev.get('packets_rx', 0)
    delta_bytes = current.get('bytes_rx', 0) - prev.get('bytes_rx', 0)

    # 归一化为速率
    syn_rate = delta_syn / interval_sec
    rst_rate = delta_rst / interval_sec
    new_conn_rate = delta_conn / interval_sec
    packet_rate = delta_packets / interval_sec
    avg_packet_size = delta_bytes / max(delta_packets, 1)

    # 归一化到合理范围 (异常检测对尺度敏感)
    return np.array([
        min(syn_rate / 1000, 1.0),           # 归一化: 0-1000 SYN/s
        min(rst_rate / 100, 1.0),            # 归一化: 0-100 RST/s
        min(new_conn_rate / 500, 1.0),       # 归一化: 0-500 conn/s
        min(packet_rate / 100000, 1.0),      # 归一化: 0-100K pkt/s
        min(avg_packet_size / 1500, 1.0),    # 归一化: 0-MTU
    ], dtype=np.float32)


def generate_anomaly_dataset(metrics: List[Dict]) -> Dict:
    """
    生成异常检测 Autoencoder 训练数据

    Autoencoder 是无监督学习:
    - 输入 = 输出 (重构目标)
    - 训练时只用正常数据
    - 推理时重构误差大 = 异常
    """
    features = []

    for i in range(1, len(metrics)):
        feat = compute_anomaly_features(metrics[i], metrics[i-1], interval_sec=0.1)
        features.append(feat)

    features = np.array(features, dtype=np.float32)

    return {
        'inputs': features,         # Autoencoder 输入
        'targets': features.copy(), # Autoencoder 目标 = 输入
    }


# ============================================================================
# 带宽预测数据集生成 (LSTM: 时序输入 → 下一时刻带宽)
# ============================================================================

def generate_bandwidth_dataset(
    samples: List[Dict],
    est_bw_list: List[float],
    history_len: int = BANDWIDTH_HISTORY_LEN
) -> Dict:
    """
    生成带宽预测 LSTM 训练数据

    输入: 过去 N 个时间步的 (throughput, rtt_ratio, loss_rate)
    输出: 下一时刻的带宽 (归一化)

    与 C++ ai_features.hpp 中的 BandwidthFeatures 保持一致
    """
    inputs = []   # [batch, history_len, 3]
    targets = []  # [batch, 1]

    for i in range(history_len, len(samples) - 1):
        # 提取历史窗口
        history = []
        for j in range(i - history_len, i):
            s = samples[j]
            est_bw = est_bw_list[j]
            min_rtt = max(s['min_rtt_us'], 1)
            packets_sent = max(s['packets_sent'], 1)

            throughput_norm = s['delivery_rate'] / MAX_BW
            rtt_ratio = s['rtt_us'] / min_rtt
            loss_rate = s['packets_lost'] / packets_sent

            history.append([throughput_norm, rtt_ratio, loss_rate])

        # 目标: 下一时刻的带宽 (归一化)
        target_bw = samples[i]['delivery_rate'] / MAX_BW

        inputs.append(history)
        targets.append([target_bw])

    return {
        'inputs': np.array(inputs, dtype=np.float32),    # [N, history_len, 3]
        'targets': np.array(targets, dtype=np.float32),  # [N, 1]
    }


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Convert NeuStack CSV to training datasets for all 3 AI models'
    )
    parser.add_argument('--tcp-samples', help='Path to tcp_samples.csv')
    parser.add_argument('--global-metrics', help='Path to global_metrics.csv')
    parser.add_argument('--output-dir', default='training/real_data/',
                        help='Output directory for datasets')
    parser.add_argument('--model', choices=['all', 'orca', 'anomaly', 'bandwidth'],
                        default='all', help='Which model dataset to generate')
    parser.add_argument('--min-samples', type=int, default=100,
                        help='Minimum samples required')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    # ─── Orca + Bandwidth: 需要 tcp_samples.csv ───
    if args.model in ['all', 'orca', 'bandwidth']:
        if not args.tcp_samples:
            print("ERROR: --tcp-samples required for orca/bandwidth models")
            return

        samples = load_tcp_samples(args.tcp_samples)
        if len(samples) < args.min_samples:
            print(f"ERROR: Need at least {args.min_samples} samples")
            return

        est_bw_list = estimate_bandwidth(samples)

        # Orca 数据集
        if args.model in ['all', 'orca']:
            print("\n[1/3] Generating Orca dataset...")
            orca_data = generate_orca_dataset(samples, est_bw_list)
            orca_path = os.path.join(args.output_dir, 'orca_dataset.npz')
            np.savez(orca_path, **orca_data)
            print(f"  Saved: {orca_path}")
            print(f"    Transitions: {len(orca_data['states'])}")
            print(f"    State dim:   {orca_data['states'].shape[1]}")
            print(f"    Reward range: [{orca_data['rewards'].min():.3f}, "
                  f"{orca_data['rewards'].max():.3f}]")

        # 带宽预测数据集
        if args.model in ['all', 'bandwidth']:
            print("\n[2/3] Generating Bandwidth Prediction dataset...")
            bw_data = generate_bandwidth_dataset(samples, est_bw_list)
            bw_path = os.path.join(args.output_dir, 'bandwidth_dataset.npz')
            np.savez(bw_path, **bw_data)
            print(f"  Saved: {bw_path}")
            print(f"    Sequences:    {len(bw_data['inputs'])}")
            print(f"    Input shape:  {bw_data['inputs'].shape}")
            print(f"    Target range: [{bw_data['targets'].min():.3f}, "
                  f"{bw_data['targets'].max():.3f}]")

    # ─── Anomaly: 需要 global_metrics.csv ───
    if args.model in ['all', 'anomaly']:
        if not args.global_metrics:
            print("\n[3/3] Skipping Anomaly dataset (no --global-metrics provided)")
        else:
            print("\n[3/3] Generating Anomaly Detection dataset...")
            metrics = load_global_metrics(args.global_metrics)
            if len(metrics) < 10:
                print("  WARNING: Too few metrics samples for anomaly detection")
            else:
                anomaly_data = generate_anomaly_dataset(metrics)
                anomaly_path = os.path.join(args.output_dir, 'anomaly_dataset.npz')
                np.savez(anomaly_path, **anomaly_data)
                print(f"  Saved: {anomaly_path}")
                print(f"    Samples:     {len(anomaly_data['inputs'])}")
                print(f"    Feature dim: {anomaly_data['inputs'].shape[1]}")

    print("\n" + "=" * 60)
    print("Dataset generation complete!")
    print(f"Output directory: {args.output_dir}")
    print("=" * 60)


if __name__ == '__main__':
    main()
```

### 9.2 转换流程

```bash
# 生成所有模型的数据集
python3 scripts/csv_to_dataset.py \
    --tcp-samples build/tcp_samples.csv \
    --global-metrics build/global_metrics.csv \
    --output-dir training/real_data/

# 或者只生成某个模型的数据集
python3 scripts/csv_to_dataset.py \
    --tcp-samples build/tcp_samples.csv \
    --model orca
```

输出示例：

```
Loaded 15234 TCP samples from build/tcp_samples.csv
Loaded 3000 global metrics from build/global_metrics.csv

[1/3] Generating Orca dataset...
  Saved: training/real_data/orca_dataset.npz
    Transitions: 15233
    State dim:   6
    Reward range: [-2.341, 0.892]

[2/3] Generating Bandwidth Prediction dataset...
  Saved: training/real_data/bandwidth_dataset.npz
    Sequences:    15223
    Input shape:  (15223, 10, 3)
    Target range: [0.012, 0.876]

[3/3] Generating Anomaly Detection dataset...
  Saved: training/real_data/anomaly_dataset.npz
    Samples:     2999
    Feature dim: 5

============================================================
Dataset generation complete!
Output directory: training/real_data/
============================================================
```

### 9.3 数据集格式说明

**orca_dataset.npz** (DDPG 离线训练):
```python
{
    'states':      np.array([N, 6], float32),   # OrcaFeatures
    'actions':     np.array([N, 1], float32),   # alpha ∈ [-1, 1]
    'rewards':     np.array([N, 1], float32),   # 奖励
    'next_states': np.array([N, 6], float32),   # 下一状态
    'dones':       np.array([N, 1], float32),   # episode 结束标志
}
```

**anomaly_dataset.npz** (Autoencoder 无监督训练):
```python
{
    'inputs':  np.array([N, 5], float32),  # AnomalyFeatures
    'targets': np.array([N, 5], float32),  # = inputs (重构目标)
}
```

**bandwidth_dataset.npz** (LSTM 监督训练):
```python
{
    'inputs':  np.array([N, 10, 3], float32),  # 历史 10 步 × 3 特征
    'targets': np.array([N, 1], float32),      # 下一时刻带宽 (归一化)
}
```

## 10. 用真实数据训练（三个模型）

### 10.1 统一训练脚本

将三个模型的训练整合到一个脚本：

```python
#!/usr/bin/env python3
"""
training/train_real.py

用真实数据训练 NeuStack 的三个 AI 模型。

用法:
    # 训练所有模型
    python train_real.py --data-dir real_data/ --all

    # 只训练 Orca
    python train_real.py --data-dir real_data/ --orca --epochs 200

    # 只训练异常检测
    python train_real.py --data-dir real_data/ --anomaly --epochs 100

    # 只训练带宽预测
    python train_real.py --data-dir real_data/ --bandwidth --epochs 100
"""

import os
import argparse
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset


# ============================================================================
# 1. Orca (DDPG 离线训练)
# ============================================================================

class OrcaActor(nn.Module):
    """Orca Actor: 6D → 1D (alpha ∈ [-1, 1])"""
    def __init__(self, state_dim=6, hidden_dim=64):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(state_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
            nn.Tanh(),
        )

    def forward(self, x):
        return self.net(x)


class OrcaCritic(nn.Module):
    """Orca Critic: (state, action) → Q value"""
    def __init__(self, state_dim=6, action_dim=1, hidden_dim=64):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(state_dim + action_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
        )

    def forward(self, state, action):
        return self.net(torch.cat([state, action], dim=-1))


def train_orca(data_path: str, output_dir: str, epochs: int, batch_size: int):
    """训练 Orca DDPG (简化的离线版本)"""
    print("\n" + "=" * 60)
    print("Training Orca (DDPG Offline)")
    print("=" * 60)

    # 加载数据
    data = np.load(data_path)
    states = torch.FloatTensor(data['states'])
    actions = torch.FloatTensor(data['actions'])
    rewards = torch.FloatTensor(data['rewards'])
    next_states = torch.FloatTensor(data['next_states'])
    dones = torch.FloatTensor(data['dones'])

    print(f"Dataset: {len(states)} transitions")

    # 创建模型
    actor = OrcaActor(state_dim=6)
    critic = OrcaCritic(state_dim=6)
    actor_target = OrcaActor(state_dim=6)
    critic_target = OrcaCritic(state_dim=6)
    actor_target.load_state_dict(actor.state_dict())
    critic_target.load_state_dict(critic.state_dict())

    actor_optim = optim.Adam(actor.parameters(), lr=1e-4)
    critic_optim = optim.Adam(critic.parameters(), lr=1e-3)

    gamma = 0.99
    tau = 0.005

    # 训练
    dataset = TensorDataset(states, actions, rewards, next_states, dones)
    loader = DataLoader(dataset, batch_size=batch_size, shuffle=True)

    for epoch in range(epochs):
        total_critic_loss = 0
        total_actor_loss = 0

        for s, a, r, s_next, d in loader:
            # Critic update
            with torch.no_grad():
                next_a = actor_target(s_next)
                target_q = r + gamma * (1 - d) * critic_target(s_next, next_a)

            current_q = critic(s, a)
            critic_loss = nn.MSELoss()(current_q, target_q)

            critic_optim.zero_grad()
            critic_loss.backward()
            critic_optim.step()

            # Actor update
            actor_loss = -critic(s, actor(s)).mean()

            actor_optim.zero_grad()
            actor_loss.backward()
            actor_optim.step()

            # Soft update targets
            for p, p_target in zip(actor.parameters(), actor_target.parameters()):
                p_target.data.copy_(tau * p.data + (1 - tau) * p_target.data)
            for p, p_target in zip(critic.parameters(), critic_target.parameters()):
                p_target.data.copy_(tau * p.data + (1 - tau) * p_target.data)

            total_critic_loss += critic_loss.item()
            total_actor_loss += actor_loss.item()

        if (epoch + 1) % 20 == 0:
            print(f"Epoch {epoch+1:4d}/{epochs} | "
                  f"Critic: {total_critic_loss/len(loader):.6f} | "
                  f"Actor: {total_actor_loss/len(loader):.6f}")

    # 保存
    os.makedirs(output_dir, exist_ok=True)
    torch.save(actor.state_dict(), os.path.join(output_dir, 'orca_actor.pth'))
    print(f"Saved: {output_dir}/orca_actor.pth")

    # 导出 ONNX
    dummy = torch.randn(1, 6)
    onnx_path = os.path.join(output_dir, 'orca_actor.onnx')
    torch.onnx.export(actor, dummy, onnx_path,
                      input_names=['state'], output_names=['action'],
                      dynamic_axes={'state': {0: 'batch'}, 'action': {0: 'batch'}},
                      opset_version=17)
    print(f"Exported: {onnx_path}")


# ============================================================================
# 2. 异常检测 (Autoencoder)
# ============================================================================

class AnomalyAutoencoder(nn.Module):
    """异常检测 Autoencoder: 5D → 5D"""
    def __init__(self, input_dim=5, hidden_dim=16):
        super().__init__()
        self.encoder = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.ReLU(),
        )
        self.decoder = nn.Sequential(
            nn.Linear(hidden_dim // 2, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, input_dim),
        )

    def forward(self, x):
        return self.decoder(self.encoder(x))


def train_anomaly(data_path: str, output_dir: str, epochs: int, batch_size: int):
    """训练异常检测 Autoencoder"""
    print("\n" + "=" * 60)
    print("Training Anomaly Detector (Autoencoder)")
    print("=" * 60)

    # 加载数据
    data = np.load(data_path)
    inputs = torch.FloatTensor(data['inputs'])
    targets = torch.FloatTensor(data['targets'])

    print(f"Dataset: {len(inputs)} samples")

    # 创建模型
    model = AnomalyAutoencoder(input_dim=5)
    optimizer = optim.Adam(model.parameters(), lr=1e-3)

    # 训练
    dataset = TensorDataset(inputs, targets)
    loader = DataLoader(dataset, batch_size=batch_size, shuffle=True)

    for epoch in range(epochs):
        total_loss = 0
        for x, y in loader:
            pred = model(x)
            loss = nn.MSELoss()(pred, y)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item()

        if (epoch + 1) % 20 == 0:
            print(f"Epoch {epoch+1:4d}/{epochs} | Loss: {total_loss/len(loader):.6f}")

    # 计算阈值 (正常数据的 95% 分位数重构误差)
    model.eval()
    with torch.no_grad():
        preds = model(inputs)
        errors = ((preds - targets) ** 2).mean(dim=1).numpy()
        threshold = np.percentile(errors, 95)
    print(f"Anomaly threshold (95th percentile): {threshold:.6f}")

    # 保存
    os.makedirs(output_dir, exist_ok=True)
    torch.save({
        'model': model.state_dict(),
        'threshold': threshold,
    }, os.path.join(output_dir, 'anomaly_detector.pth'))
    print(f"Saved: {output_dir}/anomaly_detector.pth")

    # 导出 ONNX (带 threshold 元数据)
    dummy = torch.randn(1, 5)
    onnx_path = os.path.join(output_dir, 'anomaly_detector.onnx')
    torch.onnx.export(model, dummy, onnx_path,
                      input_names=['input'], output_names=['output'],
                      dynamic_axes={'input': {0: 'batch'}, 'output': {0: 'batch'}},
                      opset_version=17)
    print(f"Exported: {onnx_path}")
    print(f"  (Threshold {threshold:.6f} needs to be stored separately or in metadata)")


# ============================================================================
# 3. 带宽预测 (LSTM → 简化为 MLP)
# ============================================================================

class BandwidthPredictor(nn.Module):
    """带宽预测: [N, 10, 3] → [N, 1]"""
    def __init__(self, history_len=10, features=3, hidden_dim=32):
        super().__init__()
        # 简化为 MLP (展平输入)
        self.net = nn.Sequential(
            nn.Flatten(),
            nn.Linear(history_len * features, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
            nn.Sigmoid(),  # 输出 [0, 1]
        )

    def forward(self, x):
        return self.net(x)


def train_bandwidth(data_path: str, output_dir: str, epochs: int, batch_size: int):
    """训练带宽预测模型"""
    print("\n" + "=" * 60)
    print("Training Bandwidth Predictor")
    print("=" * 60)

    # 加载数据
    data = np.load(data_path)
    inputs = torch.FloatTensor(data['inputs'])
    targets = torch.FloatTensor(data['targets'])

    print(f"Dataset: {len(inputs)} sequences")
    print(f"Input shape: {inputs.shape}")

    # 创建模型
    history_len = inputs.shape[1]
    features = inputs.shape[2]
    model = BandwidthPredictor(history_len=history_len, features=features)
    optimizer = optim.Adam(model.parameters(), lr=1e-3)

    # 训练
    dataset = TensorDataset(inputs, targets)
    loader = DataLoader(dataset, batch_size=batch_size, shuffle=True)

    for epoch in range(epochs):
        total_loss = 0
        for x, y in loader:
            pred = model(x)
            loss = nn.MSELoss()(pred, y)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item()

        if (epoch + 1) % 20 == 0:
            print(f"Epoch {epoch+1:4d}/{epochs} | Loss: {total_loss/len(loader):.6f}")

    # 保存
    os.makedirs(output_dir, exist_ok=True)
    torch.save(model.state_dict(), os.path.join(output_dir, 'bandwidth_predictor.pth'))
    print(f"Saved: {output_dir}/bandwidth_predictor.pth")

    # 导出 ONNX
    dummy = torch.randn(1, history_len, features)
    onnx_path = os.path.join(output_dir, 'bandwidth_predictor.onnx')
    torch.onnx.export(model, dummy, onnx_path,
                      input_names=['input'], output_names=['output'],
                      dynamic_axes={'input': {0: 'batch'}, 'output': {0: 'batch'}},
                      opset_version=17)
    print(f"Exported: {onnx_path}")


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='Train NeuStack AI models with real data')
    parser.add_argument('--data-dir', required=True, help='Directory with *_dataset.npz files')
    parser.add_argument('--output-dir', default='checkpoints/', help='Output directory')
    parser.add_argument('--epochs', type=int, default=100)
    parser.add_argument('--batch-size', type=int, default=256)

    # 模型选择
    parser.add_argument('--all', action='store_true', help='Train all models')
    parser.add_argument('--orca', action='store_true', help='Train Orca')
    parser.add_argument('--anomaly', action='store_true', help='Train Anomaly Detector')
    parser.add_argument('--bandwidth', action='store_true', help='Train Bandwidth Predictor')

    args = parser.parse_args()

    # 默认训练所有
    if not (args.orca or args.anomaly or args.bandwidth):
        args.all = True

    # Orca
    if args.all or args.orca:
        orca_data = os.path.join(args.data_dir, 'orca_dataset.npz')
        if os.path.exists(orca_data):
            train_orca(orca_data, args.output_dir, args.epochs, args.batch_size)
        else:
            print(f"WARNING: {orca_data} not found, skipping Orca")

    # 异常检测
    if args.all or args.anomaly:
        anomaly_data = os.path.join(args.data_dir, 'anomaly_dataset.npz')
        if os.path.exists(anomaly_data):
            train_anomaly(anomaly_data, args.output_dir, args.epochs, args.batch_size)
        else:
            print(f"WARNING: {anomaly_data} not found, skipping Anomaly")

    # 带宽预测
    if args.all or args.bandwidth:
        bw_data = os.path.join(args.data_dir, 'bandwidth_dataset.npz')
        if os.path.exists(bw_data):
            train_bandwidth(bw_data, args.output_dir, args.epochs, args.batch_size)
        else:
            print(f"WARNING: {bw_data} not found, skipping Bandwidth")

    print("\n" + "=" * 60)
    print("Training complete!")
    print(f"Models saved to: {args.output_dir}")
    print("=" * 60)


if __name__ == '__main__':
    main()
```

### 10.2 训练命令

```bash
cd training

# 训练所有模型
python train_real.py \
    --data-dir real_data/ \
    --output-dir checkpoints/ \
    --epochs 100

# 只训练 Orca (需要更多 epochs)
python train_real.py \
    --data-dir real_data/ \
    --orca \
    --epochs 200

# 只训练异常检测
python train_real.py \
    --data-dir real_data/ \
    --anomaly \
    --epochs 50
```

输出示例：

```
============================================================
Training Orca (DDPG Offline)
============================================================
Dataset: 15233 transitions
Epoch   20/100 | Critic: 0.012345 | Actor: -0.234567
Epoch   40/100 | Critic: 0.008765 | Actor: -0.456789
...
Saved: checkpoints/orca_actor.pth
Exported: checkpoints/orca_actor.onnx

============================================================
Training Anomaly Detector (Autoencoder)
============================================================
Dataset: 2999 samples
Epoch   20/100 | Loss: 0.045678
...
Anomaly threshold (95th percentile): 0.010234
Saved: checkpoints/anomaly_detector.pth
Exported: checkpoints/anomaly_detector.onnx

============================================================
Training Bandwidth Predictor
============================================================
Dataset: 15223 sequences
Input shape: (15223, 10, 3)
Epoch   20/100 | Loss: 0.023456
...
Saved: checkpoints/bandwidth_predictor.pth
Exported: checkpoints/bandwidth_predictor.onnx

============================================================
Training complete!
Models saved to: checkpoints/
============================================================
```

### 10.3 部署训练好的模型

```bash
# 将训练好的 ONNX 模型复制到 NeuStack models 目录
cp training/checkpoints/*.onnx models/

# 验证模型
ls -la models/
# orca_actor.onnx
# anomaly_detector.onnx
# bandwidth_predictor.onnx

# 启动 NeuStack 测试
cd build
sudo ./neustack --ip 192.168.100.2 -v
```

### 10.4 模型验证脚本

```python
#!/usr/bin/env python3
"""验证三个模型的输出是否合理"""

import numpy as np
import onnxruntime as ort

# Orca
print("=== Orca ===")
orca = ort.InferenceSession('models/orca_actor.onnx')
# 正常状态: 高吞吐, 低延迟, 无丢包
normal = np.array([[0.8, 0.1, 1.1, 0.0, 0.5, 0.8]], dtype=np.float32)
# 拥塞状态: 低吞吐, 高延迟, 有丢包
congested = np.array([[0.3, 2.0, 3.0, 0.05, 1.5, 0.9]], dtype=np.float32)

print(f"  Normal state    → alpha = {orca.run(None, {'state': normal})[0][0][0]:+.3f}")
print(f"  Congested state → alpha = {orca.run(None, {'state': congested})[0][0][0]:+.3f}")

# 异常检测
print("\n=== Anomaly Detector ===")
anomaly = ort.InferenceSession('models/anomaly_detector.onnx')
# 正常: 低 SYN/RST 率
normal = np.array([[0.01, 0.001, 0.05, 0.1, 0.8]], dtype=np.float32)
# 异常: 高 SYN 率 (SYN flood)
attack = np.array([[0.9, 0.01, 0.8, 0.3, 0.1]], dtype=np.float32)

normal_out = anomaly.run(None, {'input': normal})[0]
attack_out = anomaly.run(None, {'input': attack})[0]
print(f"  Normal traffic  → reconstruction error = {np.mean((normal - normal_out)**2):.6f}")
print(f"  Attack traffic  → reconstruction error = {np.mean((attack - attack_out)**2):.6f}")

# 带宽预测
print("\n=== Bandwidth Predictor ===")
bw = ort.InferenceSession('models/bandwidth_predictor.onnx')
# 稳定高吞吐历史
stable = np.random.uniform(0.7, 0.9, (1, 10, 3)).astype(np.float32)
# 下降趋势
declining = np.linspace([0.8, 1.0, 0.0], [0.3, 2.0, 0.1], 10).reshape(1, 10, 3).astype(np.float32)

print(f"  Stable history   → predicted BW = {bw.run(None, {'input': stable})[0][0][0]:.3f}")
print(f"  Declining trend  → predicted BW = {bw.run(None, {'input': declining})[0][0][0]:.3f}")
```

预期输出：
```
=== Orca ===
  Normal state    → alpha = +0.523   (增窗)
  Congested state → alpha = -0.678   (减窗)

=== Anomaly Detector ===
  Normal traffic  → reconstruction error = 0.001234   (小 = 正常)
  Attack traffic  → reconstruction error = 0.089012   (大 = 异常)

=== Bandwidth Predictor ===
  Stable history   → predicted BW = 0.812
  Declining trend  → predicted BW = 0.345
```

## 11. 端到端验证

### 11.1 部署新模型

```bash
# 将训练好的模型复制到 models 目录
cp training/orca/checkpoints/final_model.onnx models/orca_actor.onnx

# 启动 NeuStack (带 AI + 数据采集)
cd build
sudo ./neustack --ip 192.168.100.2 --collect -v
```

### 11.2 A/B 对比

```bash
# ─── 实验 A: 纯 CUBIC (无 AI) ───
# 修改 ai_test.cpp 或 main.cpp, 不加载 Orca 模型
wrk -t2 -c10 -d30s http://192.168.100.2/api/status
# 记录: 吞吐量, 平均 RTT, P99 RTT

# ─── 实验 B: CUBIC + Orca AI ───
# 加载训练好的模型
wrk -t2 -c10 -d30s http://192.168.100.2/api/status
# 记录: 同样的指标
```

对比的关键指标：

| 指标 | 纯 CUBIC | CUBIC + Orca | 说明 |
|------|----------|------------|------|
| 吞吐量 | baseline | ≥ baseline | AI 不应降低吞吐 |
| 平均 RTT | baseline | ≤ baseline | AI 应减少排队延迟 |
| P99 RTT | baseline | < baseline | AI 应减少尾部延迟 |
| 丢包率 | baseline | < baseline | AI 应提前减窗 |

## 12. 数据流总览（三个模型）

```
┌──────────────────────────────────────────────────────────────────────────┐
│  完整的真实数据采集与训练流程 (三个 AI 模型)                                  │
│                                                                           │
│  ① 部署                                                                   │
│  ┌──────────┐       ┌──────────┐       ┌───────────────┐                 │
│  │ 真实网卡  │ ────▶ │ TUN/utun │ ────▶ │   NeuStack     │                 │
│  │ eth0/en0 │ 路由   │ tun0     │ recv  │   TCP 数据面    │                 │
│  └──────────┘       └──────────┘       └───────┬───────┘                 │
│                                                 │                         │
│  ② 采集 (两个 CSV)                              ▼                         │
│                                        ┌───────────────┐                 │
│         tcp_samples.csv  ◀─────────────│ SampleExporter │                 │
│         global_metrics.csv ◀───────────│ MetricsExporter│                 │
│                                        └───────────────┘                 │
│                                                                           │
│  ③ 转换 (csv_to_dataset.py → 三个数据集)                                  │
│                                                                           │
│         tcp_samples.csv ──┬──────▶  orca_dataset.npz                     │
│                           │           (state, action, reward, ...)       │
│                           │                                               │
│                           └──────▶  bandwidth_dataset.npz                │
│                                       (inputs[N,10,3], targets[N,1])     │
│                                                                           │
│         global_metrics.csv ──────▶  anomaly_dataset.npz                  │
│                                       (inputs[N,5], targets[N,5])        │
│                                                                           │
│  ④ 训练 (train_real.py → 三个模型)                                        │
│                                                                           │
│         orca_dataset.npz ─────────▶  orca_actor.pth                      │
│                                       (DDPG 离线训练)                     │
│                                                                           │
│         anomaly_dataset.npz ──────▶  anomaly_detector.pth                │
│                                       (Autoencoder 无监督)                │
│                                                                           │
│         bandwidth_dataset.npz ────▶  bandwidth_predictor.pth             │
│                                       (LSTM/MLP 监督学习)                 │
│                                                                           │
│  ⑤ 导出 (PyTorch → ONNX)                                                 │
│                                                                           │
│         orca_actor.pth ───────────▶  orca_actor.onnx                     │
│         anomaly_detector.pth ─────▶  anomaly_detector.onnx               │
│         bandwidth_predictor.pth ──▶  bandwidth_predictor.onnx            │
│                                                                           │
│  ⑥ 部署回 NeuStack (IntelligencePlane 加载三个模型)                        │
│                                                                           │
│         ┌─────────────────────────────────────────────────────┐          │
│         │  IntelligencePlane                                   │          │
│         │                                                      │          │
│         │  ┌────────────────┐  ┌──────────────┐  ┌──────────┐ │          │
│         │  │ Orca           │  │ Anomaly      │  │ Bandwidth│ │          │
│         │  │ (每 10ms)       │  │ (每 1s)       │  │ (每 100ms)│ │          │
│         │  │                │  │              │  │          │ │          │
│         │  │ TCPSample ──▶  │  │ GlobalMetrics│  │ TCPSample│ │          │
│         │  │ OrcaFeatures   │  │ ──▶ Anomaly  │  │ ──▶ BW   │ │          │
│         │  │ ──▶ alpha      │  │ Features     │  │ Features │ │          │
│         │  └───────┬────────┘  └──────┬───────┘  └────┬─────┘ │          │
│         │          │                  │               │       │          │
│         │          ▼                  ▼               ▼       │          │
│         │  ┌─────────────────────────────────────────────────┐│          │
│         │  │              SPSCQueue<AIAction>                ││          │
│         │  └─────────────────────────────────────────────────┘│          │
│         └─────────────────────────────────────────────────────┘          │
│                                        │                                  │
│                                        ▼                                  │
│         ┌─────────────────────────────────────────────────────┐          │
│         │  TCP 数据面                                          │          │
│         │  • TCPOrca::set_alpha() (Orca 输出)                  │          │
│         │  • TCPOrca::set_predicted_bandwidth() (BW 预测输出)   │          │
│         │  • 异常告警日志 (Anomaly 输出)                         │          │
│         └─────────────────────────────────────────────────────┘          │
│                                                                           │
└──────────────────────────────────────────────────────────────────────────┘
```

## 13. 新增文件清单

```
include/neustack/metrics/
└── sample_exporter.hpp       # TCPSample CSV 导出器

src/hal/
└── hal_linux.cpp             # Linux TUN 设备实现

scripts/
└── csv_to_dataset.py         # CSV → .npz (三个模型的数据集)

training/
├── real_data/                # 采集的真实数据
│   ├── orca_dataset.npz
│   ├── anomaly_dataset.npz
│   └── bandwidth_dataset.npz
├── train_real.py             # 统一训练脚本 (三个模型)
└── checkpoints/              # 训练输出
    ├── orca_actor.pth
    ├── orca_actor.onnx
    ├── anomaly_detector.pth
    ├── anomaly_detector.onnx
    ├── bandwidth_predictor.pth
    └── bandwidth_predictor.onnx
```

## 14. 常见问题

### Q: NeuStack 启动后 ping 不通？

```bash
# 1. 检查虚拟网卡是否配置了 IP
ifconfig utun3  # macOS
ip addr show tun0  # Linux

# 2. 检查路由
netstat -rn | grep 192.168.100  # macOS
ip route | grep 10.0.1  # Linux

# 3. 检查 NeuStack 是否在运行
# 日志中应该看到 ICMP echo request
```

### Q: iperf3 连不上 NeuStack？

NeuStack 没有内置 iperf3 服务端。用自带的 HTTP 或 TCP Echo：
- HTTP: `curl http://192.168.100.2/`
- TCP Echo: `ncat 192.168.100.2 7`

### Q: 采集的数据量不够？

- 增加测试持续时间（`-d 300s`）
- 增加并发连接数（`-c 50`）
- 跑多次、合并 CSV

### Q: Linux 上 `/dev/net/tun` 不存在？

```bash
sudo modprobe tun
ls -la /dev/net/tun
# 如果还没有，检查内核配置: CONFIG_TUN=y
```

### Q: 采集到的 min_rtt_us 全是 0？

说明还没有收到过 ACK。确保流量是双向的（请求-响应），而不是单向发送。

## 15. 下一步

- **持续学习**: 在线收集数据 → 定期重新训练 → 热更新模型
- **多场景泛化**: 在不同网络条件下采集，混合训练
- **模型评估框架**: 自动化 A/B 测试，量化改进

## 16. 参考资料

- [Linux TUN/TAP](https://www.kernel.org/doc/html/latest/networking/tuntap.html) — TUN 设备文档
- [macOS utun](https://developer.apple.com/documentation/networkextension) — utun 参考
- [tc netem](https://man7.org/linux/man-pages/man8/tc-netem.8.html) — 网络模拟
- [Offline RL](https://arxiv.org/abs/2005.01643) — 离线强化学习综述
- [Orca Paper](https://www.usenix.org/conference/nsdi22/presentation/abbasloo) — Orca 原始论文
