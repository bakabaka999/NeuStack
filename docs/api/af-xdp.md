# AF_XDP High-Performance Backend

> **Requires:** Linux, libbpf, clang (for BPF compilation)

AF_XDP is NeuStack's high-performance data path backend for Linux. It bypasses most of the kernel network stack by mapping NIC ring buffers into userspace via shared memory (UMEM) and using a BPF/XDP program to redirect packets — reducing per-packet overhead and enabling batched ring I/O.

> **Current status:** Tested in **generic (SKB copy) mode** on a Realtek r8169 NIC. In this mode, packets still go through the kernel sk_buff path before entering UMEM — one memory copy occurs. Native zero-copy mode (`zero_copy = true`) is implemented and ready for NICs with XDP driver support (Intel i40e / ice / igc / mlx5).

## Architecture

```
┌─────────────────────────────────────┐
│           NeuStack (userspace)      │
│                                     │
│  ┌──────────┐    ┌──────────────┐  │
│  │ AF_XDP   │    │  UMEM        │  │
│  │ Socket   │◄──►│  (mmap'd     │  │
│  │          │    │   frames)    │  │
│  └────┬─────┘    └──────┬───────┘  │
│       │                 │          │
├───────┼─────────────────┼──────────┤
│       │    XDP Program  │  kernel  │
│       │    (BPF)        │          │
│       ▼                 ▼          │
│  ┌──────────────────────────────┐  │
│  │        NIC Driver            │  │
│  └──────────────────────────────┘  │
└─────────────────────────────────────┘
```

## Build

```bash
# AF_XDP requires libbpf and clang
sudo apt install libbpf-dev clang

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEUSTACK_ENABLE_AF_XDP=ON \
  -DNEUSTACK_ENABLE_AI=ON

cmake --build build -j$(nproc)
```

Verify the build:
```
-- Enable AF_XDP:    ON
--   BPF supported:  ON
```

If `BPF supported` is OFF, check that clang and libbpf are installed.

## Configuration

```cpp
#include "neustack/hal/hal_linux_afxdp.hpp"

neustack::AFXDPConfig cfg;
cfg.ifname = "eth0";            // Network interface
cfg.queue_id = 0;               // NIC queue (multi-queue NICs)
cfg.frame_count = 4096;         // UMEM frame count
cfg.frame_size = 4096;          // Frame size (bytes)
cfg.batch_size = 64;            // Batch processing size
cfg.zero_copy = true;           // Zero-copy mode (requires NIC support)
cfg.force_native_mode = false;  // true = require native XDP (fail if unsupported)
cfg.bpf_prog_path = "";         // Empty = SKB/generic mode

auto dev = std::make_unique<neustack::LinuxAFXDPDevice>(cfg);
dev->open();
```

## Modes

| Mode | Flag | NIC Requirement | Performance |
|------|------|-----------------|-------------|
| Generic (SKB) | `force_native_mode = false` | Any NIC | 1.0-1.5 Mpps |
| Native | `force_native_mode = true` | Driver XDP support | 5-10 Mpps |
| Zero-copy | `zero_copy = true` | Driver AF_XDP ZC support | 10+ Mpps |

### NIC Compatibility

| Driver | Native XDP | Zero-Copy | Chipset Examples |
|--------|-----------|-----------|------------------|
| `igc` | Yes | Yes | Intel I225-V, Killer E2600* |
| `i40e` | Yes | Yes | Intel X710, XL710 |
| `ice` | Yes | Yes | Intel E810 |
| `mlx5` | Yes | Yes | Mellanox ConnectX-5/6 |
| `r8169` | No | No | Realtek RTL8111/8125 |
| `r8152` | Partial | No | Realtek USB NICs |

*\*Some Killer E2600 models use Realtek (r8169) instead of Intel (igc). Check with `ethtool -i <iface> | grep driver`.*

## Batch API

```cpp
neustack::PacketDesc descs[64];

// Receive batch
uint32_t n = dev->recv_batch(descs, 64);
for (uint32_t i = 0; i < n; i++) {
    process(descs[i].data, descs[i].len);
}
dev->release_rx(descs, n);  // Return UMEM frames

// Send batch
uint32_t sent = dev->send_batch(descs, count);
```

## Checking Your NIC

```bash
# Find your interface
ip link show

# Check driver
sudo ethtool -i <interface> | grep driver

# Test XDP support (loads a trivial XDP program)
sudo ip link set dev <interface> xdpgeneric obj /path/to/xdp_prog.o sec xdp
sudo ip link set dev <interface> xdpgeneric off  # cleanup
```
