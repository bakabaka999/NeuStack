#ifndef NEUSTACK_HAL_LINUX_AFXDP_HPP
#define NEUSTACK_HAL_LINUX_AFXDP_HPP

#ifdef NEUSTACK_PLATFORM_LINUX

#include "neustack/hal/device.hpp"
#include "neustack/hal/umem.hpp"
#include "neustack/hal/xdp_ring.hpp"
#include <string>
#include <array>

namespace neustack {

/**
 * AF_XDP 设备配置
 */
struct AFXDPConfig {
    // ─── 网卡配置 ───
    std::string ifname = "eth0"; // 网卡名称
    uint32_t queue_id = 0;       // 网卡队列 ID (多队列网卡)

    // ─── L2 地址 (AF_XDP 工作在 L2，发送时需要填 MAC) ───
    std::array<uint8_t, 6> local_mac = {};    // 本机网卡 MAC (ip link show 获取)
    std::array<uint8_t, 6> gateway_mac = {};  // 网关 MAC (arp -n 获取)

    // ─── UMEM 配置 ───
    uint32_t frame_count = 4096; // UMEM frame 数量
    uint32_t frame_size = 4096;  // 每个 frame 大小
    uint32_t headroom = 0;       // frame 预留头部空间

    // ─── Ring 配置 ───
    uint32_t fill_ring_size = 2048; // Fill Ring 大小 (2 的幂)
    uint32_t comp_ring_size = 2048; // Completion Ring 大小
    uint32_t rx_ring_size = 2048;   // RX Ring 大小
    uint32_t tx_ring_size = 2048;   // TX Ring 大小

    // ─── 行为配置 ───
    uint32_t batch_size = 64;       // 批量收发大小
    bool zero_copy = true;          // 是否启用 zero-copy 模式
    bool busy_poll = false;         // 是否启用 busy polling (需要 root)
    uint32_t busy_poll_budget = 64; // busy poll 每次处理的包数

    // ─── XDP 程序 ───
    std::string bpf_prog_path = ""; // BPF 程序路径 (空 = 使用 SKB 模式)
    bool force_native_mode = false; // 强制 native XDP 模式 (需要网卡驱动支持)
};

class LinuxAFXDPDevice : public NetDevice {
public:
    explicit LinuxAFXDPDevice(const AFXDPConfig &config = {});
    ~LinuxAFXDPDevice() override;

    // ─── NetDevice 接口 ───

    int open() override;
    int close() override;
    ssize_t send(const uint8_t *data, size_t len) override;
    ssize_t recv(uint8_t *buf, size_t len, int timeout_ms = -1) override;
    int get_fd() const override { return _xsk_fd; }
    std::string get_name() const override { return _config.ifname; }

    // ─── v1.4 扩展接口 ───

    bool supports_zero_copy() const override { return _config.zero_copy; }
    bool supports_batch() const override { return true; }

    uint32_t recv_batch(PacketDesc *descs, uint32_t max_count) override;
    uint32_t send_batch(const PacketDesc *descs, uint32_t count) override;
    void release_rx(const PacketDesc *descs, uint32_t count) override;
    int poll(int timeout_ms) override;

    // ─── AF_XDP 专用 ───

    /** 获取 UMEM (用于直接操作 frame) */
    Umem& umem() { return _umem; }

    /** 加载 BPF/XDP 程序 */
    int load_bpf_program(const std::string& path);

    /** 获取统计信息 */
    struct Stats {
        uint64_t rx_packets;
        uint64_t tx_packets;
        uint64_t rx_dropped;        // Fill Ring 空导致的丢包
        uint64_t tx_dropped;        // TX Ring 满导致的丢包
        uint64_t fill_ring_empty;   // Fill Ring 空的次数
        uint64_t invalid_descs;     // 无效描述符
    };
    Stats stats() const { return _stats; }

private:
    AFXDPConfig _config;
    int _xsk_fd = -1;
    int _ifindex = -1;

    Umem _umem;

    // 四条 Ring
    XdpRing<uint64_t> _fill_ring;
    XdpRing<uint64_t> _comp_ring;
    XdpRing<xdp_desc> _rx_ring;
    XdpRing<xdp_desc> _tx_ring;

    // Ring 的 mmap 区域 (需要在析构时 munmap)
    void *_fill_ring_mmap = nullptr;
    void *_comp_ring_mmap = nullptr;
    void *_rx_ring_mmap = nullptr;
    void *_tx_ring_mmap = nullptr;
    size_t _fill_ring_mmap_size = 0;
    size_t _comp_ring_mmap_size = 0;
    size_t _rx_ring_mmap_size = 0;
    size_t _tx_ring_mmap_size = 0;

    // BPF
    int _bpf_prog_fd = -1;
    int _xdp_flags = 0;

    Stats _stats = {};

    // ─── 内部方法 ───

    int create_socket();
    int setup_rings();
    int bind_shto_interface();
    void populate_fill_ring();
    void reclaim_completion_ring();

    // Ring mmap 辅助
    int mmap_ring(int fd, uint64_t pgoff, size_t size,
                  void **mmap_ptr, size_t *mmap_size);

    // L2 地址自动获取
    int auto_detect_local_mac();
    int auto_detect_gateway_mac();
};

} // namespace neustack

#endif // NEUSTACK_PLATFORM_LINUX
#endif // NEUSTACK_HAL_LINUX_AFXDP_HPP