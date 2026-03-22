#!/usr/bin/env python3
"""
plot_results.py — Generate publication-quality figures from benchmark results.

Reads summary.json produced by benchmark_runner.py and generates:
  1. XDP Ring batch amortization curve
  2. Zero-copy vs traditional send path comparison
  3. Header build comparison (build vs build_header_only)
  4. Component latency waterfall
  5. Ablation study (TUN vs AF_XDP configurations)

Usage:
    python3 scripts/bench/plot_results.py \
        --input bench_results/latest/summary.json \
        --output bench_results/latest/figures/
"""

import argparse
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


# ─────────────────────────────────────────────────────────
# Style configuration: publication-quality
# ─────────────────────────────────────────────────────────

def setup_style():
    """Configure matplotlib for publication-quality output."""
    plt.rcParams.update({
        "font.family": "serif",
        "font.size": 9,
        "axes.labelsize": 10,
        "axes.titlesize": 11,
        "legend.fontsize": 8,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "figure.figsize": (3.5, 2.5),  # single-column width
        "figure.dpi": 300,
        "savefig.dpi": 300,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.05,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "grid.linewidth": 0.5,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "lines.linewidth": 1.5,
        "lines.markersize": 5,
    })


# Color palette: distinguishable, print-friendly
COLORS = ["#2196F3", "#F44336", "#4CAF50", "#FF9800", "#9C27B0", "#795548"]


# ─────────────────────────────────────────────────────────
# Helper: extract metric value (handles both flat and stat dicts)
# ─────────────────────────────────────────────────────────

def get_val(d, key, stat="mean"):
    """Navigate nested dict by dotted key, returning stat or raw value."""
    parts = key.split(".")
    cur = d
    for p in parts:
        if isinstance(cur, dict) and p in cur:
            cur = cur[p]
        else:
            return None
    if isinstance(cur, dict) and stat in cur:
        return cur[stat]
    if isinstance(cur, (int, float)):
        return cur
    return None


def get_err(d, key):
    """Get std deviation for error bars."""
    parts = key.split(".")
    cur = d
    for p in parts:
        if isinstance(cur, dict) and p in cur:
            cur = cur[p]
        else:
            return 0.0
    if isinstance(cur, dict) and "std" in cur:
        return cur["std"]
    return 0.0


# ─────────────────────────────────────────────────────────
# Figure 1: XDP Ring Batch Amortization Curve
# ─────────────────────────────────────────────────────────

def plot_xdp_ring_batch(results, output_dir):
    """Line chart: batch_size vs ns/op for XDP ring operations."""
    batch_sizes = [1, 8, 32, 64, 128]
    ns_vals = []
    err_vals = []

    xdp = results.get("xdp_ring", {})
    for bs in batch_sizes:
        key = f"batch_{bs}_ns_per_op"
        val = get_val(xdp, key)
        err = get_err(xdp, key)
        if val is not None:
            ns_vals.append(val)
            err_vals.append(err)
        else:
            ns_vals.append(0)
            err_vals.append(0)

    if all(v == 0 for v in ns_vals):
        print("  [Skip] XDP Ring batch: no data (Linux-only test)")
        return

    fig, ax = plt.subplots()
    ax.errorbar(batch_sizes, ns_vals, yerr=err_vals,
                marker="o", color=COLORS[0], capsize=3, capthick=1)
    ax.set_xlabel("Batch Size")
    ax.set_ylabel("Amortized Latency (ns/op)")
    ax.set_title("XDP Ring: Batch Amortization")
    ax.set_xscale("log", base=2)
    ax.set_xticks(batch_sizes)
    ax.set_xticklabels([str(b) for b in batch_sizes])

    save_fig(fig, output_dir, "01_xdp_ring_batch")


# ─────────────────────────────────────────────────────────
# Figure 2: Zero-Copy vs Traditional Send Path
# ─────────────────────────────────────────────────────────

def plot_zero_copy(results, output_dir):
    """Grouped bar chart: traditional vs zero-copy."""
    zc = results.get("zero_copy", {})
    trad = get_val(zc, "traditional_ns_per_pkt")
    zero = get_val(zc, "zero_copy_ns_per_pkt")
    trad_err = get_err(zc, "traditional_ns_per_pkt")
    zero_err = get_err(zc, "zero_copy_ns_per_pkt")

    if trad is None or zero is None:
        print("  [Skip] Zero-copy: no data")
        return

    speedup = trad / zero if zero > 0 else 0

    fig, ax = plt.subplots()
    x = [0, 1]
    bars = ax.bar(x, [trad, zero], yerr=[trad_err, zero_err],
                  color=[COLORS[1], COLORS[0]], capsize=5, width=0.5,
                  edgecolor="black", linewidth=0.5)
    ax.set_xticks(x)
    ax.set_xticklabels(["Traditional\n(3 copies)", "Zero-Copy\n(1 copy)"])
    ax.set_ylabel("Latency (ns/pkt)")
    ax.set_title("Send Path: Zero-Copy vs Traditional")

    # Annotate speedup
    ax.annotate(f"{speedup:.1f}x faster",
                xy=(1, zero), xytext=(1.3, (trad + zero) / 2),
                arrowprops=dict(arrowstyle="->", color="black"),
                fontsize=8, fontweight="bold", ha="left")

    save_fig(fig, output_dir, "02_zero_copy_vs_traditional")


# ─────────────────────────────────────────────────────────
# Figure 3: Header Build Comparison
# ─────────────────────────────────────────────────────────

def plot_header_build(results, output_dir):
    """Grouped bar chart: build vs build_header_only for TCP and IPv4."""
    hb = results.get("header_build", {})

    tcp_build = get_val(hb, "tcp_build_ns_per_op")
    tcp_hdr = get_val(hb, "tcp_header_only_ns_per_op")
    ipv4_build = get_val(hb, "ipv4_build_ns_per_op")
    ipv4_hdr = get_val(hb, "ipv4_header_only_ns_per_op")

    if any(v is None for v in [tcp_build, tcp_hdr, ipv4_build, ipv4_hdr]):
        print("  [Skip] Header build: incomplete data")
        return

    tcp_build_err = get_err(hb, "tcp_build_ns_per_op")
    tcp_hdr_err = get_err(hb, "tcp_header_only_ns_per_op")
    ipv4_build_err = get_err(hb, "ipv4_build_ns_per_op")
    ipv4_hdr_err = get_err(hb, "ipv4_header_only_ns_per_op")

    fig, ax = plt.subplots()

    x = np.array([0, 1])
    width = 0.3

    build_vals = [tcp_build, ipv4_build]
    build_errs = [tcp_build_err, ipv4_build_err]
    hdr_vals = [tcp_hdr, ipv4_hdr]
    hdr_errs = [tcp_hdr_err, ipv4_hdr_err]

    ax.bar(x - width / 2, build_vals, width, yerr=build_errs,
           label="build()", color=COLORS[1], capsize=3,
           edgecolor="black", linewidth=0.5)
    ax.bar(x + width / 2, hdr_vals, width, yerr=hdr_errs,
           label="build_header_only()", color=COLORS[0], capsize=3,
           edgecolor="black", linewidth=0.5)

    # Annotate savings
    for i, (full, hdr) in enumerate(zip(build_vals, hdr_vals)):
        pct = (full - hdr) / full * 100
        ax.text(i, max(full, hdr) + 3, f"-{pct:.0f}%", ha="center",
                fontsize=7, fontweight="bold", color=COLORS[2])

    ax.set_xticks(x)
    ax.set_xticklabels(["TCP", "IPv4"])
    ax.set_ylabel("Latency (ns/op)")
    ax.set_title("Header Construction: Full Build vs Header-Only")
    ax.legend(loc="upper right", framealpha=0.9)

    save_fig(fig, output_dir, "03_header_build_comparison")


# ─────────────────────────────────────────────────────────
# Figure 4: Component Latency Waterfall
# ─────────────────────────────────────────────────────────

def plot_waterfall(results, output_dir):
    """Horizontal stacked bar chart showing per-component latency."""
    # Collect representative latencies from each component
    components = []

    # UMEM alloc (use sequential as representative)
    umem = results.get("umem_alloc_free", {})
    v = get_val(umem, "sequential_ns_per_op")
    if v is not None:
        components.append(("UMEM\nAlloc/Free", v))

    # Ring ops (use batch=1 as representative)
    xdp = results.get("xdp_ring", {})
    v = get_val(xdp, "batch_1_ns_per_op")
    if v is not None:
        components.append(("XDP Ring\nOps", v))

    # Header build (TCP build as representative)
    hb = results.get("header_build", {})
    v = get_val(hb, "tcp_build_ns_per_op")
    if v is not None:
        components.append(("Header\nBuild", v))

    # Metrics
    gm = results.get("global_metrics", {})
    v = get_val(gm, "aligned_ns_per_op")
    if v is not None:
        components.append(("Metrics\nUpdate", v))

    if len(components) < 2:
        print("  [Skip] Waterfall: insufficient data")
        return

    labels = [c[0] for c in components]
    values = [c[1] for c in components]
    total = sum(values)

    fig, ax = plt.subplots(figsize=(5, 1.5 + 0.3 * len(components)))

    # Horizontal stacked bar
    left = 0
    for i, (label, val) in enumerate(components):
        pct = val / total * 100
        bar = ax.barh(0, val, left=left, color=COLORS[i % len(COLORS)],
                      edgecolor="black", linewidth=0.5, height=0.5)
        # Label inside the bar
        cx = left + val / 2
        ax.text(cx, 0, f"{label}\n{val:.1f} ns\n({pct:.0f}%)",
                ha="center", va="center", fontsize=6, fontweight="bold")
        left += val

    ax.set_xlim(0, total * 1.05)
    ax.set_xlabel("Latency (ns)")
    ax.set_title("Per-Packet Processing: Component Latency Breakdown")
    ax.set_yticks([])

    save_fig(fig, output_dir, "04_component_waterfall")


# ─────────────────────────────────────────────────────────
# Figure 5: Ablation Study (placeholder for real-machine data)
# ─────────────────────────────────────────────────────────

def plot_ablation(results, output_dir):
    """Grouped bar chart for ablation study configurations.

    Expected keys in results:
      ablation.tun_baseline_mpps
      ablation.afxdp_copy_mpps
      ablation.afxdp_zerocopy_mpps
      ablation.afxdp_zerocopy_ai_mpps
    """
    ab = results.get("ablation", {})
    configs = [
        ("TUN\nBaseline", "tun_baseline_mpps"),
        ("AF_XDP\n(copy)", "afxdp_copy_mpps"),
        ("AF_XDP\n(zero-copy)", "afxdp_zerocopy_mpps"),
        ("AF_XDP+ZC\n+AI", "afxdp_zerocopy_ai_mpps"),
    ]

    labels = []
    values = []
    errors = []
    for label, key in configs:
        v = get_val(ab, key)
        if v is not None:
            labels.append(label)
            values.append(v)
            errors.append(get_err(ab, key))

    if len(values) < 2:
        print("  [Skip] Ablation: no data (run on real machine)")
        return

    fig, ax = plt.subplots(figsize=(4, 2.8))
    x = np.arange(len(labels))
    bars = ax.bar(x, values, yerr=errors, color=COLORS[:len(values)],
                  capsize=4, edgecolor="black", linewidth=0.5, width=0.6)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Throughput (Mpps)")
    ax.set_title("Ablation: Data Path Configuration Comparison")

    # Annotate values on top
    for bar, val in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.02,
                f"{val:.2f}", ha="center", va="bottom", fontsize=7)

    save_fig(fig, output_dir, "05_ablation_study")


# ─────────────────────────────────────────────────────────
# LaTeX table generation
# ─────────────────────────────────────────────────────────

def generate_latex_table(results, output_dir):
    """Generate a LaTeX summary table from benchmark results."""
    lines = []
    lines.append(r"\begin{table}[htbp]")
    lines.append(r"\centering")
    lines.append(r"\caption{AF\_XDP Datapath Micro-Benchmark Results}")
    lines.append(r"\label{tab:afxdp-bench}")
    lines.append(r"\begin{tabular}{lrrr}")
    lines.append(r"\toprule")
    lines.append(r"Component & Metric & Mean & Std \\")
    lines.append(r"\midrule")

    # Collect rows from results
    row_specs = [
        ("UMEM Alloc", "umem_alloc_free.sequential_ns_per_op", "ns/op"),
        ("UMEM Batch", "umem_alloc_free.batch_ns_per_op", "ns/op"),
        ("XDP Ring (b=1)", "xdp_ring.batch_1_ns_per_op", "ns/op"),
        ("XDP Ring (b=32)", "xdp_ring.batch_32_ns_per_op", "ns/op"),
        ("XDP Ring (b=128)", "xdp_ring.batch_128_ns_per_op", "ns/op"),
        ("Send: Traditional", "zero_copy.traditional_ns_per_pkt", "ns/pkt"),
        ("Send: Zero-Copy", "zero_copy.zero_copy_ns_per_pkt", "ns/pkt"),
        ("TCP build()", "header_build.tcp_build_ns_per_op", "ns/op"),
        ("TCP header\\_only()", "header_build.tcp_header_only_ns_per_op", "ns/op"),
        ("IPv4 build()", "header_build.ipv4_build_ns_per_op", "ns/op"),
        ("IPv4 header\\_only()", "header_build.ipv4_header_only_ns_per_op", "ns/op"),
        ("Metrics (aligned)", "global_metrics.aligned_ns_per_op", "ns/op"),
        ("Metrics (packed)", "global_metrics.unaligned_ns_per_op", "ns/op"),
    ]

    for label, key, unit in row_specs:
        mean = get_val(results, key, "mean")
        std = get_val(results, key, "std")
        if mean is not None:
            std_str = f"{std:.2f}" if std is not None else "--"
            lines.append(f"  {label} & {unit} & {mean:.2f} & {std_str} \\\\")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")

    tex_path = os.path.join(output_dir, "benchmark_table.tex")
    with open(tex_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  LaTeX table: {tex_path}")


# ─────────────────────────────────────────────────────────
# Utility
# ─────────────────────────────────────────────────────────

def save_fig(fig, output_dir, name):
    """Save figure as both PDF and PNG."""
    for ext in ["pdf", "png"]:
        path = os.path.join(output_dir, f"{name}.{ext}")
        fig.savefig(path)
    plt.close(fig)
    print(f"  Saved: {name}.pdf / .png")


def load_results(input_path: str) -> dict:
    """Load and normalize results from summary.json.

    Handles both single-benchmark stats files and combined summary.json.
    Returns a flat results dict.
    """
    with open(input_path) as f:
        data = json.load(f)

    # Combined summary from benchmark_runner.py
    if "benchmarks" in data:
        # Merge results from all benchmarks
        merged = {}
        for bench_name, bench_data in data["benchmarks"].items():
            if "results" in bench_data:
                merged.update(bench_data["results"])
        return merged

    # Single benchmark stats file
    if "results" in data:
        return data["results"]

    return data


def main():
    parser = argparse.ArgumentParser(
        description="Generate publication-quality benchmark figures")
    parser.add_argument("--input", required=True,
                        help="Path to summary.json or stats JSON file")
    parser.add_argument("--output", default=None,
                        help="Output directory for figures (default: figures/ next to input)")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: input file '{args.input}' not found.", file=sys.stderr)
        sys.exit(1)

    output_dir = args.output or os.path.join(os.path.dirname(args.input), "figures")
    os.makedirs(output_dir, exist_ok=True)

    setup_style()

    print(f"=== NeuStack Benchmark Plotter ===")
    print(f"  Input:  {args.input}")
    print(f"  Output: {output_dir}")
    print()

    results = load_results(args.input)

    plot_xdp_ring_batch(results, output_dir)
    plot_zero_copy(results, output_dir)
    plot_header_build(results, output_dir)
    plot_waterfall(results, output_dir)
    plot_ablation(results, output_dir)
    generate_latex_table(results, output_dir)

    print(f"\n=== Done ===")


if __name__ == "__main__":
    main()
