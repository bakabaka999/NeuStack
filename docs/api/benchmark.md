# Benchmark Framework

NeuStack includes a comprehensive benchmark framework for measuring component-level and end-to-end performance.

## Quick Start

```bash
# Build (Release mode required for accurate results)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DNEUSTACK_BUILD_BENCHMARKS=ON \
  -DNEUSTACK_ENABLE_AF_XDP=ON -DNEUSTACK_ENABLE_AI=ON
cmake --build build --parallel

# Run micro-benchmarks (5 rounds, with statistics)
python3 scripts/bench/benchmark_runner.py \
  --build-dir build/ --runs 5 --benchmarks bench_afxdp_datapath

# Generate figures
python3 scripts/bench/plot_results.py \
  --input bench_results/latest/summary.json

# Run E2E throughput test (requires root)
sudo bash scripts/bench/run_throughput_test.sh --duration 10 --runs 3
```

## Components

### 1. Micro-Benchmark: `bench_afxdp_datapath`

Tests individual data path components in isolation:

| Test | What It Measures |
|------|-----------------|
| UMEM Alloc/Free | Frame allocator throughput (sequential + batch) |
| XDP Ring Ops | Ring buffer throughput across batch sizes (1-128) |
| Zero-Copy Path | Traditional 3-copy vs zero-copy 1-copy send path |
| Prefetch Effect | Cache prefetch impact on packet buffer access |
| Header Build | `build()` vs `build_header_only()` for TCP/IPv4 |
| Global Metrics | Atomic counter hot path (aligned vs packed) |

Supports `--json` for structured output:

```bash
./build/tests/bench_afxdp_datapath --json | python3 -m json.tool
```

### 2. E2E Throughput: `bench_e2e_throughput`

Measures real packet receive throughput using veth pairs and network namespaces:

```
┌── ns_sink ──┐   veth pair   ┌── ns_gen ──┐
│             │               │            │
│  Receiver   │◄── veth0/1 ──►│  UDP Flood │
│ (backend)   │               │ (sendmmsg) │
└─────────────┘               └────────────┘
```

Three backends compared:

| Backend | Socket Type | Description |
|---------|-------------|-------------|
| `kernel_udp` | `SOCK_DGRAM` | Full kernel stack (IP → UDP → socket buffer → copy) |
| `raw_socket` | `AF_PACKET` | L2 capture, per-packet `recvfrom()` (TUN-equivalent) |
| `af_xdp` | AF_XDP | Ring buffer batch receive (NeuStack's fast path) |

### 3. Benchmark Runner: `benchmark_runner.py`

Runs benchmarks N times and computes statistics:

```bash
python3 scripts/bench/benchmark_runner.py \
  --build-dir build/ \
  --runs 5 \
  --benchmarks bench_afxdp_datapath \
  --output bench_results/
```

Output: `bench_results/<timestamp>/summary.json` with mean, std, min, max, p50, p95 per metric.

### 4. Plot Generator: `plot_results.py`

Generates publication-quality figures (PDF + PNG) and a LaTeX table:

```bash
python3 scripts/bench/plot_results.py \
  --input bench_results/latest/summary.json
```

| Figure | Description |
|--------|-------------|
| `01_xdp_ring_batch` | Ring batch amortization curve (ns/op vs batch size) |
| `02_zero_copy_vs_traditional` | Zero-copy vs 3-copy send path comparison |
| `03_header_build_comparison` | `build()` vs `build_header_only()` |
| `04_component_waterfall` | Per-packet latency breakdown by component |
| `05_ablation_study` | Configuration comparison (ablation data) |
| `benchmark_table.tex` | LaTeX table for papers |

**Dependencies:** `pip install numpy matplotlib` (see `scripts/bench/requirements.txt`)

### 5. Ablation Script: `run_ablation.sh`

Builds and tests multiple cmake configurations:

```bash
sudo bash scripts/bench/run_ablation.sh --runs 5
```

Configurations: TUN baseline → AF_XDP (copy) → AF_XDP (zero-copy) → AF_XDP + AI

### 6. UDP Flood Tool: `udp_flood`

High-speed packet generator using `sendmmsg()` (64 packets per syscall):

```bash
./build/tools/udp_flood <target_ip> <port> <payload_size> <duration_s>
```

## Results

### Micro-Benchmarks (Intel i5, Ubuntu 22.04, GCC 13.3, `-O2`)

| Component | Metric | Value |
|-----------|--------|-------|
| UMEM alloc+free (sequential) | ns/op | 0.46 |
| UMEM alloc+free (batch 4096) | ns/op | 0.68 |
| XDP Ring (batch=1) | ns/op | 2.37 |
| XDP Ring (batch=128) | ns/op | 0.55 |
| Send: traditional (3 copies) | ns/pkt | 162 |
| Send: zero-copy (1 copy) | ns/pkt | 52 |
| Zero-copy speedup | | **3.1×** |
| TCP `build()` | ns/op | 11.4 |
| TCP `build_header_only()` | ns/op | 1.9 |
| Header-only speedup | | **6×** |

### E2E Throughput (veth, generic XDP, Realtek r8169)

| Backend | Mpps | vs Kernel |
|---------|------|-----------|
| Kernel UDP (`SOCK_DGRAM`) | 0.82 | baseline |
| Raw socket (`AF_PACKET`, TUN-equivalent) | 0.60 | 0.73× |
| **AF_XDP (generic/SKB)** | **1.18** | **1.45×** |

> [!NOTE]
> These results use generic XDP (SKB mode) on a NIC without native XDP support. Native XDP on Intel NICs (i40e/ice/igc) is expected to yield 5–10× additional throughput gains.

## Reproducing

```bash
# Full pipeline
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DNEUSTACK_BUILD_BENCHMARKS=ON \
  -DNEUSTACK_ENABLE_AF_XDP=ON -DNEUSTACK_ENABLE_AI=ON
cmake --build build --parallel

# Micro-benchmarks
python3 scripts/bench/benchmark_runner.py --build-dir build/ --runs 5
python3 scripts/bench/plot_results.py --input bench_results/latest/summary.json
ls bench_results/latest/figures/  # PDF + PNG + LaTeX

# E2E throughput
sudo bash scripts/bench/run_throughput_test.sh --duration 10 --runs 3
cat bench_results/latest_e2e/summary.json | python3 -m json.tool
```
