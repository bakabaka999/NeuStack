#ifdef NEUSTACK_PLATFORM_LINUX

// src/hal/hal_linux_afxdp.cpp

#include "neustack/hal/hal_linux_afxdp.hpp"
#include "neustack/hal/ethernet.hpp"
#include "neustack/common/log.hpp"

#include <linux/if_xdp.h>
#include <linux/if_link.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <net/if.h> // if_nametoindex
#include <sys/ioctl.h>
#include <linux/if.h> // ifreq (SIOCGIFHWADDR)
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#ifdef NEUSTACK_ENABLE_AF_XDP
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#endif

using namespace neustack;

LinuxAFXDPDevice::LinuxAFXDPDevice(const AFXDPConfig& config)
    : _config(config)
    , _umem(Umem::Config{config.frame_count, config.frame_size, config.headroom})
{}

LinuxAFXDPDevice::~LinuxAFXDPDevice() {
    close();
}

int LinuxAFXDPDevice::open() {
    // 1. 查找网卡接口索引
    _ifindex = if_nametoindex(_config.ifname.c_str());
    if (_ifindex == 0) {
        LOG_ERROR(HAL, "Interface not found: %s", _config.ifname.c_str());
        return -1;
    }

    // 1.5 自动获取本机 MAC (如果用户没配置)
    static const std::array<uint8_t,6> zero_mac = {};
    if (_config.local_mac == zero_mac) {
        if (auto_detect_local_mac() != 0) {
            LOG_WARN(HAL, "Failed to auto-detect local MAC, send path may not work");
        }
    }
    // 自动获取网关 MAC (如果用户没配置)
    if (_config.gateway_mac == zero_mac) {
        if (auto_detect_gateway_mac() != 0) {
            LOG_WARN(HAL, "Failed to auto-detect gateway MAC, send path may not work");
        }
    }

    // 2. 分配 UMEM 内存
    if (_umem.create() != 0) {
        LOG_ERROR(HAL, "Failed to create UMEM");
        return -1;
    }

    // 3. 创建 XDP socket
    if (create_socket() != 0) {
        LOG_ERROR(HAL, "Failed to create XDP socket");
        return -1;
    }

    // 4. 设置 Ring Buffer
    if (setup_rings() != 0) {
        LOG_ERROR(HAL, "Failed to setup rings");
        return -1;
    }

    // 5. 绑定到网卡
    if (bind_to_interface() != 0) {
        LOG_ERROR(HAL, "Failed to bind to interface");
        return -1;
    }

    // 5.1 配置 busy polling (低延迟模式)
    if (_config.busy_poll) {
        int val = static_cast<int>(_config.busy_poll_budget);
        if (setsockopt(_xsk_fd, SOL_SOCKET, SO_BUSY_POLL, &val, sizeof(val)) < 0) {
            LOG_WARN(HAL, "SO_BUSY_POLL failed: %s (need root?)", strerror(errno));
        } else {
            LOG_INFO(HAL, "Busy polling enabled (budget=%d)", val);
        }
#ifdef SO_PREFER_BUSY_POLL
        int prefer = 1;
        setsockopt(_xsk_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));
#endif
    }

    // 5.5 加载 BPF/XDP 重定向程序 (socket 必须先绑定才能注册到 XSKMAP)
    {
        std::string bpf_path = _config.bpf_prog_path;
#ifdef NEUSTACK_BPF_OBJECT_DIR
        if (bpf_path.empty()) {
            // Use the build-time compiled BPF object
            bpf_path = std::string(NEUSTACK_BPF_OBJECT_DIR) + "/xdp_redirect.o";
        }
#endif
        if (!bpf_path.empty()) {
            if (load_bpf_program(bpf_path) != 0) {
                LOG_WARN(HAL, "BPF program load failed; AF_XDP may not receive packets without an XDP program");
            }
        } else {
            LOG_INFO(HAL, "No BPF program configured — relying on external XDP setup");
        }
    }

    // 6. 初始填充 Fill Ring
    populate_fill_ring();

    LOG_INFO(HAL, "AF_XDP device opened: %s queue=%u frames=%u",
             _config.ifname.c_str(), _config.queue_id, _config.frame_count);

    return 0;
}

int LinuxAFXDPDevice::close() {
    // 卸载 BPF 程序
#ifdef NEUSTACK_ENABLE_AF_XDP
    if (_bpf_prog_fd >= 0 && _ifindex > 0) {
        bpf_xdp_detach(_ifindex, _xdp_flags, nullptr);
        _bpf_prog_fd = -1;
    }
    if (_bpf_obj) {
        bpf_object__close(static_cast<struct bpf_object*>(_bpf_obj));
        _bpf_obj = nullptr;
    }
#else
    if (_bpf_prog_fd >= 0) {
        ::close(_bpf_prog_fd);
        _bpf_prog_fd = -1;
    }
#endif

    // munmap 所有 Ring
    if (_fill_ring_mmap) { munmap(_fill_ring_mmap, _fill_ring_mmap_size); _fill_ring_mmap = nullptr; }
    if (_comp_ring_mmap) { munmap(_comp_ring_mmap, _comp_ring_mmap_size); _comp_ring_mmap = nullptr; }
    if (_rx_ring_mmap) { munmap(_rx_ring_mmap, _rx_ring_mmap_size); _rx_ring_mmap = nullptr; }
    if (_tx_ring_mmap) { munmap(_tx_ring_mmap, _tx_ring_mmap_size); _tx_ring_mmap = nullptr; }

    // 关闭 socket
    if (_xsk_fd >= 0) {
        ::close(_xsk_fd);
        _xsk_fd = -1;
    }

    // UMEM 在 Umem 析构时自动 munmap

    return 0;
}

ssize_t LinuxAFXDPDevice::send(const uint8_t* data, size_t len) {
    PacketDesc desc;
    desc.data = const_cast<uint8_t*>(data);
    desc.len = static_cast<uint32_t>(len);
    desc.addr = 0;
    desc.flags = 0;  // 非 zero-copy，send_batch 内部会拷贝到 UMEM

    uint32_t sent = send_batch(&desc, 1);
    return (sent > 0) ? static_cast<ssize_t>(len) : -1;
}

ssize_t LinuxAFXDPDevice::recv(uint8_t* buf, size_t len, int timeout_ms) {
    // 等待数据
    if (timeout_ms != 0) {
        int events = poll(timeout_ms);
        if (!(events & POLL_RX)) return 0;
    }

    // 接收一个包
    PacketDesc desc;
    uint32_t n = recv_batch(&desc, 1);
    if (n == 0) return 0;

    // 拷贝到用户缓冲区 (兼容模式)
    size_t copy_len = std::min(len, static_cast<size_t>(desc.len));
    memcpy(buf, desc.data, copy_len);

    // 释放 frame
    release_rx(&desc, 1);

    return static_cast<ssize_t>(copy_len);
}

uint32_t LinuxAFXDPDevice::recv_batch(PacketDesc* descs, uint32_t max_count) {
    // 1. 先回收已完成的 TX frame
    reclaim_completion_ring();

    // 2. 主动补充 Fill Ring（水位策略）
    refill_fill_ring();

    // 3. 从 RX Ring 读取
    uint32_t n = _rx_ring.peek(max_count);
    if (n == 0) {
        // RX 为空，检查是否需要唤醒内核填充
        if (_fill_ring.needs_wakeup()) {
            recvfrom(_xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        }
        return 0;
    }

    uint32_t out = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& desc = _rx_ring.ring_at_consumer(i);
        uint8_t* frame = _umem.addr_to_ptr(desc.addr);

        // 剥离 Ethernet header，过滤非 IPv4
        const uint8_t* payload;
        uint32_t payload_len;
        uint16_t etype = parse_ethernet(frame, desc.len, &payload, &payload_len);
        if (etype != ETH_P_IP) {
            // 非 IPv4 (ARP, IPv6 等)，释放 frame
            _umem.free_frame(desc.addr);
            continue;
        }

        descs[out].data = const_cast<uint8_t*>(payload);
        descs[out].len = payload_len;
        descs[out].addr = static_cast<uint32_t>(desc.addr);
        descs[out].port_id = 0;
        descs[out].flags = PacketDesc::FLAG_ZEROCOPY | PacketDesc::FLAG_NEED_FREE;
        ++out;
    }

    _rx_ring.release(n);
    _stats.rx_packets += out;

    // 被过滤的包已 free_frame，补充 Fill Ring 防止饥饿
    if (out < n) {
        uint32_t dropped = n - out;
        uint32_t refill = _fill_ring.reserve(dropped);
        uint32_t filled = 0;
        for (uint32_t i = 0; i < refill; ++i) {
            uint64_t addr = _umem.alloc_frame();
            if (addr == UmemFrameAllocator::INVALID_ADDR) break;
            _fill_ring.ring_at(i) = addr;
            ++filled;
        }
        _fill_ring.submit(filled);
    }

    return out;
}

uint32_t LinuxAFXDPDevice::send_batch(const PacketDesc* descs, uint32_t count) {
    uint32_t n = _tx_ring.reserve(count);
    if (n == 0) {
        _stats.tx_dropped += count;
        return 0;
    }

    const uint8_t* dst_mac = _config.gateway_mac.data();
    const uint8_t* src_mac = _config.local_mac.data();

    for (uint32_t i = 0; i < n; ++i) {
        auto& tx_desc = _tx_ring.ring_at(i);

        if (descs[i].is_zerocopy()) {
            // 数据已经在 UMEM 中，调用者负责 Ethernet header
            tx_desc.addr = descs[i].addr;
            tx_desc.len = descs[i].len;
        } else {
            // L3 数据在外部缓冲区，拷贝到 UMEM frame 并添加 Ethernet header
            uint64_t frame_addr = _umem.alloc_frame();
            if (frame_addr == UmemFrameAllocator::INVALID_ADDR) {
                n = i;
                break;
            }
            uint8_t* frame = _umem.addr_to_ptr(frame_addr);

            // 写入 Ethernet header
            build_ethernet(frame + ETH_HLEN, dst_mac, src_mac, ETH_P_IP);

            // 拷贝 L3 payload
            memcpy(frame + ETH_HLEN, descs[i].data, descs[i].len);

            tx_desc.addr = frame_addr;
            tx_desc.len = descs[i].len + ETH_HLEN;
        }

        tx_desc.options = 0;
    }

    _tx_ring.submit(n);
    _stats.tx_packets += n;

    // 通知内核有数据要发送 (如果需要唤醒)
    if (_tx_ring.needs_wakeup()) {
        sendto(_xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    }

    return n;
}

void LinuxAFXDPDevice::release_rx(const PacketDesc* descs, uint32_t count) {
    // 将处理完的 frame 归还 Fill Ring，让内核重新使用
    uint32_t n = _fill_ring.reserve(count);

    for (uint32_t i = 0; i < n; ++i) {
        _fill_ring.ring_at(i) = descs[i].addr;
    }

    _fill_ring.submit(n);

    // 如果 Fill Ring 满了，放回 allocator (下次再填)
    for (uint32_t i = n; i < count; ++i) {
        _umem.free_frame(descs[i].addr);
    }

    // 主动补充 Fill Ring（水位策略）
    refill_fill_ring();

    // 补充后检查是否需要唤醒内核
    if (_fill_ring.needs_wakeup()) {
        recvfrom(_xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    }
}

int LinuxAFXDPDevice::poll(int timeout_ms) {
    struct pollfd pfd = {};
    pfd.fd = _xsk_fd;
    pfd.events = POLLIN | POLLOUT;

    int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret < 0) return POLL_ERROR;
    if (ret == 0) return 0;

    int events = 0;
    if (pfd.revents & POLLIN) events |= POLL_RX;
    if (pfd.revents & POLLOUT) events |= POLL_TX;
    if (pfd.revents & POLLERR) events |= POLL_ERROR;
    return events;
}

int LinuxAFXDPDevice::create_socket() {
    _xsk_fd = socket(AF_XDP, SOCK_RAW, 0);
    if (_xsk_fd < 0) {
        LOG_ERROR(HAL, "socket(AF_XDP) failed: %s", strerror(errno));
        return -1;
    }

    // 注册 UMEM
    if (_umem.register_to_socket(_xsk_fd) != 0) {
        LOG_ERROR(HAL, "UMEM registration failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int LinuxAFXDPDevice::mmap_ring(int fd, uint64_t pgoff, size_t size,
                                void** mmap_ptr, size_t* mmap_size) {
    *mmap_size = size;
    *mmap_ptr = mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE,
                     fd, pgoff);
    if (*mmap_ptr == MAP_FAILED) {
        *mmap_ptr = nullptr;
        LOG_ERROR(HAL, "mmap ring failed (pgoff=0x%lx): %s",
                  (unsigned long)pgoff, strerror(errno));
        return -1;
    }
    return 0;
}

int LinuxAFXDPDevice::setup_rings() {
    // 1. 设置 Fill Ring 大小
    int opt = _config.fill_ring_size;
    if (setsockopt(_xsk_fd, SOL_XDP, XDP_UMEM_FILL_RING, &opt, sizeof(opt)) < 0) {
        LOG_ERROR(HAL, "Set fill ring size failed: %s", strerror(errno));
        return -1;
    }

    // 2. 设置 Completion Ring 大小
    opt = _config.comp_ring_size;
    if (setsockopt(_xsk_fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &opt, sizeof(opt)) < 0) {
        LOG_ERROR(HAL, "Set completion ring size failed: %s", strerror(errno));
        return -1;
    }

    // 3. 设置 RX Ring 大小
    opt = _config.rx_ring_size;
    if (setsockopt(_xsk_fd, SOL_XDP, XDP_RX_RING, &opt, sizeof(opt)) < 0) {
        LOG_ERROR(HAL, "Set RX ring size failed: %s", strerror(errno));
        return -1;
    }

    // 4. 设置 TX Ring 大小
    opt = _config.tx_ring_size;
    if (setsockopt(_xsk_fd, SOL_XDP, XDP_TX_RING, &opt, sizeof(opt)) < 0) {
        LOG_ERROR(HAL, "Set TX ring size failed: %s", strerror(errno));
        return -1;
    }

    // 5. 获取 Ring 的 mmap 偏移量
    struct xdp_mmap_offsets offsets;
    socklen_t optlen = sizeof(offsets);
    if (getsockopt(_xsk_fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &optlen) < 0) {
        LOG_ERROR(HAL, "Get mmap offsets failed: %s", strerror(errno));
        return -1;
    }

    // 6. mmap 所有 Ring
    // Fill Ring
    if (mmap_ring(_xsk_fd, XDP_UMEM_PGOFF_FILL_RING,
                  offsets.fr.desc + _config.fill_ring_size * sizeof(uint64_t),
                  &_fill_ring_mmap, &_fill_ring_mmap_size) < 0)
        return -1;

    _fill_ring.init(
        reinterpret_cast<uint32_t*>((uint8_t*)_fill_ring_mmap + offsets.fr.producer),
        reinterpret_cast<uint32_t*>((uint8_t*)_fill_ring_mmap + offsets.fr.consumer),
        reinterpret_cast<uint64_t*>((uint8_t*)_fill_ring_mmap + offsets.fr.desc),
        _config.fill_ring_size,
        reinterpret_cast<uint32_t*>((uint8_t*)_fill_ring_mmap + offsets.fr.flags)
    );

    // Completion Ring
    if (mmap_ring(_xsk_fd, XDP_UMEM_PGOFF_COMPLETION_RING,
                  offsets.cr.desc + _config.comp_ring_size * sizeof(uint64_t),
                  &_comp_ring_mmap, &_comp_ring_mmap_size) < 0)
        return -1;

    _comp_ring.init(
        reinterpret_cast<uint32_t*>((uint8_t*)_comp_ring_mmap + offsets.cr.producer),
        reinterpret_cast<uint32_t*>((uint8_t*)_comp_ring_mmap + offsets.cr.consumer),
        reinterpret_cast<uint64_t*>((uint8_t*)_comp_ring_mmap + offsets.cr.desc),
        _config.comp_ring_size,
        reinterpret_cast<uint32_t*>((uint8_t*)_comp_ring_mmap + offsets.cr.flags)
    );

    // RX Ring
    if (mmap_ring(_xsk_fd, XDP_PGOFF_RX_RING,
                  offsets.rx.desc + _config.rx_ring_size * sizeof(struct xdp_desc),
                  &_rx_ring_mmap, &_rx_ring_mmap_size) < 0)
        return -1;

    _rx_ring.init(
        reinterpret_cast<uint32_t*>((uint8_t*)_rx_ring_mmap + offsets.rx.producer),
        reinterpret_cast<uint32_t*>((uint8_t*)_rx_ring_mmap + offsets.rx.consumer),
        reinterpret_cast<struct xdp_desc*>((uint8_t*)_rx_ring_mmap + offsets.rx.desc),
        _config.rx_ring_size,
        reinterpret_cast<uint32_t*>((uint8_t*)_rx_ring_mmap + offsets.rx.flags)
    );

    // TX Ring
    if (mmap_ring(_xsk_fd, XDP_PGOFF_TX_RING,
                  offsets.tx.desc + _config.tx_ring_size * sizeof(struct xdp_desc),
                  &_tx_ring_mmap, &_tx_ring_mmap_size) < 0)
        return -1;

    _tx_ring.init(
        reinterpret_cast<uint32_t*>((uint8_t*)_tx_ring_mmap + offsets.tx.producer),
        reinterpret_cast<uint32_t*>((uint8_t*)_tx_ring_mmap + offsets.tx.consumer),
        reinterpret_cast<struct xdp_desc*>((uint8_t*)_tx_ring_mmap + offsets.tx.desc),
        _config.tx_ring_size,
        reinterpret_cast<uint32_t*>((uint8_t*)_tx_ring_mmap + offsets.tx.flags)
    );

    return 0;
}

int LinuxAFXDPDevice::bind_to_interface() {
    struct sockaddr_xdp sxdp = {};
    sxdp.sxdp_family = PF_XDP;
    sxdp.sxdp_ifindex = _ifindex;
    sxdp.sxdp_queue_id = _config.queue_id;

    // 选择绑定模式
    if (_config.zero_copy) {
        sxdp.sxdp_flags = XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP;
    } else {
        sxdp.sxdp_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
    }

    if (bind(_xsk_fd, reinterpret_cast<struct sockaddr*>(&sxdp), sizeof(sxdp)) < 0) {
        if (errno == ENOPROTOOPT && _config.zero_copy) {
            // 网卡不支持 zero-copy，回退到 copy 模式
            LOG_WARN(HAL, "Zero-copy not supported, falling back to copy mode");
            sxdp.sxdp_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
            if (bind(_xsk_fd, reinterpret_cast<struct sockaddr*>(&sxdp),
                     sizeof(sxdp)) < 0) {
                LOG_ERROR(HAL, "bind(AF_XDP copy) failed: %s", strerror(errno));
                return -1;
            }
        } else {
            LOG_ERROR(HAL, "bind(AF_XDP) failed: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

void LinuxAFXDPDevice::populate_fill_ring() {
    // 预填充一半的 frame 到 Fill Ring
    uint32_t fill_count = _config.frame_count / 2;
    uint32_t n = _fill_ring.reserve(fill_count);

    uint32_t filled = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t addr = _umem.alloc_frame();
        if (addr == UmemFrameAllocator::INVALID_ADDR) break;
        _fill_ring.ring_at(i) = addr;
        ++filled;
    }

    _fill_ring.submit(filled);
    LOG_DEBUG(HAL, "Fill ring populated with %u frames", filled);
}

void LinuxAFXDPDevice::refill_fill_ring() {
    // 水位策略：当 Fill Ring 可用空间 > 3/4 时主动补充到 1/2
    uint32_t avail = _fill_ring.reserve(0);  // 探测可用空间（不占用）
    uint32_t threshold = _config.fill_ring_size / 4;

    if (avail >= threshold || _umem.available_frames() == 0) {
        return;  // 水位足够 或 没有空闲 frame
    }

    _stats.fill_ring_empty++;

    // 补充到 ring_size / 2
    uint32_t target = _config.fill_ring_size / 2;
    uint32_t need = target - avail;
    uint32_t n = _fill_ring.reserve(need);

    uint32_t filled = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t addr = _umem.alloc_frame();
        if (addr == UmemFrameAllocator::INVALID_ADDR) break;
        _fill_ring.ring_at(i) = addr;
        ++filled;
    }

    _fill_ring.submit(filled);
}

void LinuxAFXDPDevice::reclaim_completion_ring() {
    uint32_t n = _comp_ring.peek(_config.batch_size);
    if (n == 0) return;

    for (uint32_t i = 0; i < n; ++i) {
        uint64_t addr = _comp_ring.ring_at_consumer(i);
        _umem.free_frame(addr);  // 归还到空闲池
    }

    _comp_ring.release(n);
}

int LinuxAFXDPDevice::load_bpf_program(const std::string& path) {
#ifdef NEUSTACK_ENABLE_AF_XDP
    // 1. Open BPF ELF object
    struct bpf_object* obj = bpf_object__open(path.c_str());
    if (!obj) {
        LOG_ERROR(HAL, "bpf_object__open(%s) failed: %s", path.c_str(), strerror(errno));
        return -1;
    }

    // 2. Load programs + maps into kernel
    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(HAL, "bpf_object__load failed: %s", strerror(errno));
        bpf_object__close(obj);
        return -1;
    }

    // 3. Find the XDP program and get its fd
    struct bpf_program* prog = bpf_object__find_program_by_name(obj, "xdp_sock_prog");
    if (!prog) {
        // Fallback: find first program in the object
        prog = bpf_object__next_program(obj, nullptr);
    }
    if (!prog) {
        LOG_ERROR(HAL, "No XDP program found in %s", path.c_str());
        bpf_object__close(obj);
        return -1;
    }

    int prog_fd = bpf_program__fd(prog);
    if (prog_fd < 0) {
        LOG_ERROR(HAL, "bpf_program__fd failed: %s", strerror(errno));
        bpf_object__close(obj);
        return -1;
    }

    // 4. Attach XDP program to the interface
    //    Use bpf_set_link_xdp_fd (compatible with libbpf >= 0.5)
    _xdp_flags = 0;
    if (_config.force_native_mode) {
        _xdp_flags = XDP_FLAGS_DRV_MODE;
    } else {
        _xdp_flags = XDP_FLAGS_SKB_MODE;
    }

    if (bpf_xdp_attach(_ifindex, prog_fd, _xdp_flags, nullptr) < 0) {
        // Retry without mode flag (some older kernels)
        LOG_WARN(HAL, "XDP attach with flags=%d failed, retrying generic", _xdp_flags);
        _xdp_flags = 0;
        if (bpf_xdp_attach(_ifindex, prog_fd, _xdp_flags, nullptr) < 0) {
            LOG_ERROR(HAL, "bpf_xdp_attach failed: %s", strerror(errno));
            bpf_object__close(obj);
            return -1;
        }
    }

    // 5. Find xsks_map and register our socket
    struct bpf_map* map = bpf_object__find_map_by_name(obj, "xsks_map");
    if (!map) {
        LOG_ERROR(HAL, "xsks_map not found in BPF object");
        bpf_xdp_detach(_ifindex, _xdp_flags, nullptr);
        bpf_object__close(obj);
        return -1;
    }

    int map_fd = bpf_map__fd(map);
    int key = static_cast<int>(_config.queue_id);
    int xsk_fd = _xsk_fd;

    if (bpf_map_update_elem(map_fd, &key, &xsk_fd, BPF_ANY) != 0) {
        LOG_ERROR(HAL, "bpf_map_update_elem(xsks_map[%d]) failed: %s",
                  key, strerror(errno));
        bpf_xdp_detach(_ifindex, _xdp_flags, nullptr);
        bpf_object__close(obj);
        return -1;
    }

    _bpf_prog_fd = prog_fd;
    _bpf_obj = obj;

    LOG_INFO(HAL, "BPF/XDP program loaded: %s (prog_fd=%d, queue=%d, flags=0x%x)",
             path.c_str(), prog_fd, key, _xdp_flags);
    return 0;

#else
    LOG_WARN(HAL, "AF_XDP support not compiled in; cannot load BPF program: %s", path.c_str());
    return -1;
#endif
}

int LinuxAFXDPDevice::auto_detect_local_mac() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, _config.ifname.c_str(), IFNAMSIZ - 1);

    int ret = ioctl(fd, SIOCGIFHWADDR, &ifr);
    ::close(fd);
    if (ret < 0) {
        LOG_ERROR(HAL, "ioctl(SIOCGIFHWADDR) failed: %s", strerror(errno));
        return -1;
    }

    memcpy(_config.local_mac.data(), ifr.ifr_hwaddr.sa_data, 6);
    LOG_INFO(HAL, "Auto-detected local MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             _config.local_mac[0], _config.local_mac[1], _config.local_mac[2],
             _config.local_mac[3], _config.local_mac[4], _config.local_mac[5]);
    return 0;
}

int LinuxAFXDPDevice::auto_detect_gateway_mac() {
    // 1. 从 /proc/net/route 找默认网关 IP
    FILE* fp = fopen("/proc/net/route", "r");
    if (!fp) {
        LOG_ERROR(HAL, "Cannot open /proc/net/route");
        return -1;
    }

    char line[256];
    uint32_t gw_ip = 0;
    fgets(line, sizeof(line), fp); // 跳过表头
    while (fgets(line, sizeof(line), fp)) {
        char iface[IFNAMSIZ];
        uint32_t dest, gateway;
        if (sscanf(line, "%s %x %x", iface, &dest, &gateway) == 3) {
            if (dest == 0 && strcmp(iface, _config.ifname.c_str()) == 0) {
                gw_ip = gateway; // 已经是网络字节序
                break;
            }
        }
    }
    fclose(fp);

    if (gw_ip == 0) {
        LOG_WARN(HAL, "No default gateway found for %s", _config.ifname.c_str());
        return -1;
    }

    // 2. 从 /proc/net/arp 查网关 MAC
    fp = fopen("/proc/net/arp", "r");
    if (!fp) {
        LOG_ERROR(HAL, "Cannot open /proc/net/arp");
        return -1;
    }

    // 把网关 IP 转成点分十进制用于匹配
    uint8_t* g = reinterpret_cast<uint8_t*>(&gw_ip);
    char gw_str[16];
    snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", g[0], g[1], g[2], g[3]);

    fgets(line, sizeof(line), fp); // 跳过表头
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char ip[32], hw[32], dev[IFNAMSIZ];
        unsigned type, flags;
        // IP address  HW type  Flags  HW address  Mask  Device
        if (sscanf(line, "%31s 0x%x 0x%x %31s %*s %s", ip, &type, &flags, hw, dev) == 5) {
            if (strcmp(ip, gw_str) == 0) {
                unsigned int m[6];
                if (sscanf(hw, "%x:%x:%x:%x:%x:%x",
                           &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6) {
                    for (int i = 0; i < 6; ++i)
                        _config.gateway_mac[i] = static_cast<uint8_t>(m[i]);
                    found = true;
                }
                break;
            }
        }
    }
    fclose(fp);

    if (!found) {
        LOG_WARN(HAL, "Gateway %s not in ARP cache (try: ping -c1 gateway first)", gw_str);
        return -1;
    }

    LOG_INFO(HAL, "Auto-detected gateway MAC: %02x:%02x:%02x:%02x:%02x:%02x (gw=%s)",
             _config.gateway_mac[0], _config.gateway_mac[1], _config.gateway_mac[2],
             _config.gateway_mac[3], _config.gateway_mac[4], _config.gateway_mac[5],
             gw_str);
    return 0;
}

#endif // NEUSTACK_PLATFORM_LINUX