#!/usr/bin/env python3
"""
benchmark_runner.py — Multi-run benchmark executor with statistical analysis.

Runs each bench_* executable N times with --json, collects results,
computes statistics (mean, std, min, max, p50, p95), and saves a
summary JSON for downstream visualization.

Usage:
    python3 scripts/bench/benchmark_runner.py \
        --build-dir build/ \
        --runs 5 \
        --output bench_results/
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from glob import glob
from pathlib import Path

import numpy as np


def discover_benchmarks(build_dir: str) -> list[str]:
    """Find all bench_* executables under build_dir/tests/."""
    pattern = os.path.join(build_dir, "tests", "bench_*")
    exes = sorted(
        p for p in glob(pattern)
        if os.path.isfile(p) and os.access(p, os.X_OK)
    )
    return exes


def run_single(exe: str) -> dict | None:
    """Run a single benchmark with --json and return parsed output."""
    try:
        result = subprocess.run(
            [exe, "--json"],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if result.returncode != 0:
            print(f"  WARNING: {os.path.basename(exe)} exited with code {result.returncode}",
                  file=sys.stderr)
            if result.stderr:
                print(f"  stderr: {result.stderr.strip()}", file=sys.stderr)
            return None
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        print(f"  WARNING: {os.path.basename(exe)} produced invalid JSON: {e}",
              file=sys.stderr)
        return None
    except subprocess.TimeoutExpired:
        print(f"  WARNING: {os.path.basename(exe)} timed out after 300s",
              file=sys.stderr)
        return None


def collect_numeric_paths(obj, prefix=""):
    """Recursively collect all numeric leaf values as flat key→value pairs."""
    items = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            new_key = f"{prefix}.{k}" if prefix else k
            items.update(collect_numeric_paths(v, new_key))
    elif isinstance(obj, (int, float)):
        items[prefix] = obj
    return items


def compute_statistics(all_runs: list[dict]) -> dict:
    """Compute per-metric statistics across multiple runs.

    Each run is assumed to have the same JSON structure.
    Returns a dict with the same key hierarchy, where each leaf is
    replaced by {mean, std, min, max, p50, p95}.
    """
    # Flatten each run's results into key→value maps
    flat_runs = []
    for run_data in all_runs:
        results = run_data.get("results", run_data)
        flat_runs.append(collect_numeric_paths(results))

    # Gather all keys seen across runs
    all_keys = set()
    for flat in flat_runs:
        all_keys.update(flat.keys())

    stats = {}
    for key in sorted(all_keys):
        values = [flat[key] for flat in flat_runs if key in flat]
        if not values:
            continue
        arr = np.array(values, dtype=np.float64)
        stats[key] = {
            "mean": float(np.mean(arr)),
            "std": float(np.std(arr, ddof=1)) if len(arr) > 1 else 0.0,
            "min": float(np.min(arr)),
            "max": float(np.max(arr)),
            "p50": float(np.percentile(arr, 50)),
            "p95": float(np.percentile(arr, 95)),
            "n": len(arr),
        }

    return stats


def unflatten(flat_stats: dict) -> dict:
    """Convert flat dotted keys back to nested dict structure."""
    result = {}
    for key, val in flat_stats.items():
        parts = key.split(".")
        d = result
        for p in parts[:-1]:
            d = d.setdefault(p, {})
        d[parts[-1]] = val
    return result


def main():
    parser = argparse.ArgumentParser(description="NeuStack benchmark runner")
    parser.add_argument("--build-dir", default="build/",
                        help="Build directory containing bench executables")
    parser.add_argument("--runs", type=int, default=5,
                        help="Number of runs per benchmark (default: 5)")
    parser.add_argument("--output", default="bench_results/",
                        help="Output directory for results")
    parser.add_argument("--benchmarks", nargs="*",
                        help="Specific benchmark names to run (default: all)")
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    if not os.path.isdir(build_dir):
        print(f"Error: build directory '{build_dir}' not found.", file=sys.stderr)
        sys.exit(1)

    # Discover benchmarks
    exes = discover_benchmarks(build_dir)
    if args.benchmarks:
        filter_set = set(args.benchmarks)
        exes = [e for e in exes if os.path.basename(e) in filter_set]

    if not exes:
        print("Error: no matching bench_* executables found.", file=sys.stderr)
        sys.exit(1)

    # Create output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = os.path.join(os.path.abspath(args.output), timestamp)
    os.makedirs(output_dir, exist_ok=True)

    print(f"=== NeuStack Benchmark Runner ===")
    print(f"  Build dir:  {build_dir}")
    print(f"  Runs:       {args.runs}")
    print(f"  Output:     {output_dir}")
    print(f"  Benchmarks: {[os.path.basename(e) for e in exes]}")
    print()

    all_summaries = {}

    for exe in exes:
        name = os.path.basename(exe)
        print(f"--- {name} ({args.runs} runs) ---")

        run_results = []
        for run_idx in range(args.runs):
            print(f"  Run {run_idx + 1}/{args.runs}...", end=" ", flush=True)
            t0 = time.monotonic()
            data = run_single(exe)
            elapsed = time.monotonic() - t0
            if data is not None:
                run_results.append(data)
                print(f"OK ({elapsed:.1f}s)")
            else:
                print(f"FAILED ({elapsed:.1f}s)")

        if not run_results:
            print(f"  WARNING: all runs failed for {name}, skipping.\n")
            continue

        # Save raw runs
        raw_path = os.path.join(output_dir, f"{name}_raw.json")
        with open(raw_path, "w") as f:
            json.dump(run_results, f, indent=2)

        # Compute and save statistics
        flat_stats = compute_statistics(run_results)
        nested_stats = unflatten(flat_stats)

        summary = {
            "benchmark": name,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "num_runs": len(run_results),
            "results": nested_stats,
        }
        all_summaries[name] = summary

        stats_path = os.path.join(output_dir, f"{name}_stats.json")
        with open(stats_path, "w") as f:
            json.dump(summary, f, indent=2)

        print(f"  Stats saved: {stats_path}\n")

    # Save combined summary
    combined = {
        "generated": datetime.now(timezone.utc).isoformat(),
        "num_runs": args.runs,
        "benchmarks": all_summaries,
    }
    summary_path = os.path.join(output_dir, "summary.json")
    with open(summary_path, "w") as f:
        json.dump(combined, f, indent=2)

    # Create/update "latest" symlink
    latest_link = os.path.join(os.path.abspath(args.output), "latest")
    if os.path.islink(latest_link):
        os.unlink(latest_link)
    elif os.path.exists(latest_link):
        os.remove(latest_link)
    os.symlink(output_dir, latest_link)

    print(f"=== Done ===")
    print(f"  Summary: {summary_path}")
    print(f"  Latest:  {latest_link}")


if __name__ == "__main__":
    main()
