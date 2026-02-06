"""
scripts/csv_to_dataset.py

将 NeuStack 采集的 CSV 转换为三个 AI 模型的训练数据集。

支持 6 组数据采集场景:
  场景 1: 本机普通 (local_normal)             -> Orca + Bandwidth
  场景 2: 本机 + tc (local_tc)                -> Orca + Bandwidth
  场景 3: 服务器普通 (server_normal)          -> Orca + Bandwidth
  场景 4: 服务器 + tc (server_tc)             -> Orca + Bandwidth
  场景 5: Anomaly short (local_anomaly_short) -> Anomaly
  场景 6: Anomaly mixed (local_anomaly_mix)   -> Anomaly

用法:
    # 从 collected_data 目录自动识别并生成所有数据集
    python scripts/csv_to_dataset.py --data-dir collected_data/

    # 只生成某个模型
    python scripts/csv_to_dataset.py --data-dir collected_data/ --model orca

输出:
    training/real_data/
    ├── orca_dataset.npz         # Orca DDPG 训练数据
    ├── bandwidth_dataset.npz    # 带宽预测 LSTM 训练数据
    └── anomaly_dataset.npz      # 异常检测 Autoencoder 训练数据
"""

import argparse
import csv
import os
import glob
import numpy as np
from typing import List, Dict


# ============================================================================
# 常量
# ============================================================================

MSS = 1460
MAX_BW = 10e6  # 10 MB/s (归一化用)
BANDWIDTH_HISTORY_LEN = 30  # 带宽预测的历史窗口

# Orca/Bandwidth 使用的 TCP 样本文件 (场景 1-4)
ORCA_BW_PATTERNS = [
    'tcp_samples_local_normal*.csv',
    'tcp_samples_local_tc*.csv',
    'tcp_samples_server_normal*.csv',
    'tcp_samples_server_tc*.csv',
]

# Anomaly 使用的 global_metrics 文件 (全部 6 组)
ANOMALY_PATTERNS = [
    'global_metrics_local_normal*.csv',
    'global_metrics_local_tc*.csv',
    'global_metrics_server_normal*.csv',
    'global_metrics_server_tc*.csv',
    'global_metrics_local_anomaly_short*.csv',
    'global_metrics_local_anomaly_mix*.csv',
]

# Anomaly 聚合窗口 (将 100ms 采样聚合为 1s)
ANOMALY_AGGREGATE_WINDOW = 10


# ============================================================================
# 通用工具函数
# ============================================================================

def find_files_by_patterns(data_dir: str, patterns: List[str]) -> List[str]:
    """在 data_dir 中按 pattern 列表查找所有匹配的文件"""
    found = []
    for pattern in patterns:
        matches = glob.glob(os.path.join(data_dir, pattern))
        matches.sort()
        found.extend(matches)
    return found


def merge_tcp_samples(*csv_files) -> List[Dict]:
    """合并多个 tcp_samples*.csv 文件"""
    all_samples = []
    for csv_file in csv_files:
        samples = []
        with open(csv_file) as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    sample = {
                        'timestamp_us': int(row['timestamp_us']),
                        'delivery_rate': float(row['delivery_rate']),
                        'send_rate': float(row.get('send_rate', 0)),
                        'rtt_us': float(row['rtt_us']),
                        'min_rtt_us': float(row['min_rtt_us']),
                        'srtt_us': float(row['srtt_us']),
                        'packets_sent': int(row['packets_sent']),
                        'packets_lost': int(row['packets_lost']),
                        'cwnd': int(row['cwnd']),
                        'bytes_in_flight': int(row.get('bytes_in_flight', 0)),
                        'loss_detected': int(row.get('loss_detected', 0)),
                        'timeout_occurred': int(row.get('timeout_occurred', 0)),
                    }
                    samples.append(sample)
                except (ValueError, KeyError):
                    continue
        all_samples.extend(samples)
        print(f"    {os.path.basename(csv_file):50s} {len(samples):>8d} samples")

    print(f"  Total: {len(all_samples)} TCP samples merged")
    return all_samples


def merge_global_metrics(*csv_files) -> List[Dict]:
    """合并多个 global_metrics*.csv 文件"""
    all_metrics = []
    for csv_file in csv_files:
        metrics = []
        with open(csv_file) as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    metrics.append({
                        'timestamp_ms': int(row.get('timestamp_ms', 0)),
                        'packets_rx': int(row['packets_rx']),
                        'packets_tx': int(row['packets_tx']),
                        'bytes_rx': int(row['bytes_rx']),
                        'bytes_tx': int(row['bytes_tx']),
                        'syn_received': int(row.get('syn_received', 0)),
                        'rst_received': int(row.get('rst_received', 0)),
                        'conn_established': int(row.get('conn_established', 0)),
                        'conn_reset': int(row.get('conn_reset', 0)),
                        'active_connections': int(row['active_connections']),
                    })
                except (ValueError, KeyError):
                    continue
        all_metrics.extend(metrics)
        active = sum(1 for m in metrics if any(m[k] != 0 for k in
                     ['packets_rx', 'packets_tx', 'syn_received', 'rst_received']))
        print(f"    {os.path.basename(csv_file):50s} {len(metrics):>6d} rows ({active} active)")

    print(f"  Total: {len(all_metrics)} metrics rows merged")
    return all_metrics


def aggregate_metrics(metrics: List[Dict], window: int = ANOMALY_AGGREGATE_WINDOW) -> List[Dict]:
    """
    将 100ms 粒度的 metrics 聚合为 1s 粒度

    delta 值 (packets, bytes, syn, rst 等): 求和
    瞬时值 (active_connections): 取窗口末尾
    """
    aggregated = []
    for i in range(0, len(metrics) - window + 1, window):
        chunk = metrics[i:i + window]
        agg = {
            'packets_rx': sum(m['packets_rx'] for m in chunk),
            'packets_tx': sum(m['packets_tx'] for m in chunk),
            'bytes_rx': sum(m['bytes_rx'] for m in chunk),
            'bytes_tx': sum(m['bytes_tx'] for m in chunk),
            'syn_received': sum(m['syn_received'] for m in chunk),
            'rst_received': sum(m['rst_received'] for m in chunk),
            'conn_established': sum(m['conn_established'] for m in chunk),
            'conn_reset': sum(m['conn_reset'] for m in chunk),
            'active_connections': chunk[-1]['active_connections'],
        }
        aggregated.append(agg)
    return aggregated


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


def compute_windowed_min_rtt(samples: List[Dict], window: int = 100, percentile: int = 10) -> List[float]:
    """
    计算窗口化的 min_rtt (基准 RTT)
    使用 srtt_us 的滑动窗口 P10 分位数作为 min_rtt 基线。
    """
    windowed = []
    for i, s in enumerate(samples):
        start = max(0, i - window)
        window_srtts = [
            samples[j]['srtt_us']
            for j in range(start, i + 1)
            if samples[j]['srtt_us'] > 0
        ]
        if window_srtts:
            base_rtt = np.percentile(window_srtts, percentile)
            windowed.append(max(base_rtt, 1))
        else:
            windowed.append(max(s['rtt_us'], 1))
    return windowed


# ============================================================================
# Orca 数据集生成 (DDPG: state, action, reward, next_state, done)
# ============================================================================

def compute_orca_features(sample: Dict, est_bw: float, min_rtt: float) -> np.ndarray:
    """从 TCPSample 提取 7 维 OrcaFeatures (6 + est_bw_norm)"""
    rtt = sample['rtt_us']
    delivery_rate = sample['delivery_rate']
    cwnd = max(sample['cwnd'], 1)
    packets_sent = max(sample['packets_sent'], 1)

    bdp = (est_bw * min_rtt / 1e6) / MSS if min_rtt > 0 else 1
    bdp = max(bdp, 1)

    throughput_norm = np.clip(delivery_rate / max(est_bw, 1), 0, 2)
    queuing_delay_norm = np.clip((rtt - min_rtt) / max(min_rtt, 1), 0, 5)
    rtt_ratio = np.clip(rtt / max(min_rtt, 1), 1, 5)
    loss_rate = np.clip(sample['packets_lost'] / packets_sent, 0, 1)
    cwnd_norm = np.clip(cwnd / max(bdp, 1), 0, 10)
    in_flight_ratio = np.clip(sample['bytes_in_flight'] / max(cwnd * MSS, 1), 0, 2)
    est_bw_norm = np.clip(est_bw / MAX_BW, 0, 2)

    return np.array([
        throughput_norm,
        queuing_delay_norm,
        rtt_ratio,
        loss_rate,
        cwnd_norm,
        in_flight_ratio,
        est_bw_norm,
    ], dtype=np.float32)


def compute_orca_reward(sample: Dict, est_bw: float, min_rtt: float) -> float:
    """Orca 奖励函数: log(吞吐) - 延迟惩罚 - 丢包惩罚"""
    rtt = sample['rtt_us']
    queuing_delay = max(0, rtt - min_rtt)

    utilization = sample['delivery_rate'] / max(est_bw, 1)
    throughput_reward = 3.0 * np.log(1 + utilization)

    queuing_delay_ratio = np.clip(queuing_delay / max(min_rtt, 1), 0, 5)
    delay_penalty = 0.3 * np.sqrt(queuing_delay_ratio)

    loss_rate = sample['packets_lost'] / max(sample['packets_sent'], 1)
    loss_penalty = 5.0 * loss_rate

    reward = throughput_reward - delay_penalty - loss_penalty
    return np.clip(reward, -3, 2)


def generate_orca_dataset(samples: List[Dict], est_bw_list: List[float], min_rtt_list: List[float]) -> Dict:
    """
    生成 Orca DDPG 训练数据

    对稳态样本 (cwnd 不变) 做降采样，保留 10%，
    避免 81% no_change 淹没有意义的窗口调整信号。
    """
    states, actions, rewards, next_states, dones = [], [], [], [], []
    kept_change = 0
    kept_stable = 0
    skipped_stable = 0

    for i in range(len(samples) - 1):
        s = samples[i]
        s_next = samples[i + 1]

        cwnd_old = max(s['cwnd'], 1)
        cwnd_new = max(s_next['cwnd'], 1)
        alpha = np.log2(cwnd_new / cwnd_old)
        alpha = np.clip(alpha, -1, 1)

        # Episode 边界
        time_gap = s_next['timestamp_us'] - s['timestamp_us']
        done = time_gap > 5_000_000

        # 降采样: cwnd 不变的样本只保留 10%
        is_stable = abs(alpha) < 0.01
        if is_stable and not done:
            if np.random.random() > 0.1:
                skipped_stable += 1
                continue
            kept_stable += 1
        else:
            kept_change += 1

        est_bw = est_bw_list[i]
        min_rtt = min_rtt_list[i]

        state = compute_orca_features(s, est_bw, min_rtt)
        next_state = compute_orca_features(s_next, est_bw_list[i + 1], min_rtt_list[i + 1])
        reward = compute_orca_reward(s, est_bw, min_rtt)

        states.append(state)
        actions.append([alpha])
        rewards.append([reward])
        next_states.append(next_state)
        dones.append([float(done)])

    print(f"    Downsampling: {kept_change} changed + {kept_stable} stable kept, "
          f"{skipped_stable} stable skipped")

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

def compute_anomaly_features(m: Dict) -> np.ndarray:
    """
    从 1 秒聚合的 GlobalMetrics 提取 8 维 AnomalyFeatures

    归一化范围基于 1 秒聚合后的预期值域
    """
    packets_rx = m.get('packets_rx', 0)
    packets_tx = m.get('packets_tx', 0)
    bytes_tx = m.get('bytes_tx', 0)
    syn_received = m.get('syn_received', 0)
    rst_received = m.get('rst_received', 0)
    conn_established = m.get('conn_established', 0)
    active_connections = m.get('active_connections', 0)

    total_packets = packets_rx + packets_tx
    tx_rx_ratio = packets_tx / max(packets_rx, 1) if total_packets > 0 else 0

    return np.array([
        np.clip(packets_rx / 20000, 0, 1),          # 收包速率 (1s)
        np.clip(packets_tx / 20000, 0, 1),           # 发包速率 (1s)
        np.clip(bytes_tx / 30000000, 0, 1),          # 发送字节速率 (1s)
        np.clip(syn_received / 100, 0, 1),           # SYN 速率 (1s)
        np.clip(rst_received / 100, 0, 1),           # RST 速率 (1s)
        np.clip(conn_established / 100, 0, 1),       # 新建连接速率 (1s)
        np.clip(tx_rx_ratio / 10, 0, 1),             # 发送/接收包比率
        np.clip(active_connections / 100, 0, 1),     # 活跃连接数
    ], dtype=np.float32)


def generate_anomaly_dataset(metrics: List[Dict]) -> Dict:
    """
    生成异常检测 Autoencoder 训练数据

    1. 将 100ms metrics 聚合为 1s 粒度
    2. 过滤空闲行
    3. 提取 8 维特征
    """
    print(f"    Raw metrics rows: {len(metrics)}")

    aggregated = aggregate_metrics(metrics)
    print(f"    After 1s aggregation: {len(aggregated)}")

    features = []
    skipped = 0
    for m in aggregated:
        feat = compute_anomaly_features(m)
        if feat.sum() == 0:
            skipped += 1
            continue
        features.append(feat)

    print(f"    After filtering idle: {len(features)} kept, {skipped} skipped")
    features = np.array(features, dtype=np.float32)

    return {
        'inputs': features,
        'targets': features.copy(),
    }


# ============================================================================
# 带宽预测数据集生成 (LSTM: 时序输入 -> 下一时刻带宽)
# ============================================================================

def generate_bandwidth_dataset(
    samples: List[Dict],
    est_bw_list: List[float],
    min_rtt_list: List[float],
    history_len: int = BANDWIDTH_HISTORY_LEN
) -> Dict:
    """
    生成带宽预测 LSTM 训练数据

    输入: 过去 N 个时间步的 (throughput, rtt_ratio, loss_rate)
    输出: 下一时刻的带宽 (归一化)
    """
    inputs = []
    targets = []

    for i in range(history_len, len(samples) - 1):
        # 检查 episode 边界: 窗口内不能跨连接
        window_start = i - history_len
        is_continuous = True
        for j in range(window_start, i):
            gap = samples[j + 1]['timestamp_us'] - samples[j]['timestamp_us']
            if gap > 5_000_000:
                is_continuous = False
                break
        if not is_continuous:
            continue

        history = []
        for j in range(window_start, i):
            s = samples[j]
            min_rtt = max(min_rtt_list[j], 1)
            packets_sent = max(s['packets_sent'], 1)

            throughput_norm = np.clip(s['delivery_rate'] / MAX_BW, 0, 1)
            rtt_ratio = np.clip(s['rtt_us'] / min_rtt, 1, 5)
            loss_rate = np.clip(s['packets_lost'] / packets_sent, 0, 1)

            history.append([throughput_norm, rtt_ratio, loss_rate])

        target_bw = np.clip(samples[i]['delivery_rate'] / MAX_BW, 0, 1)

        inputs.append(history)
        targets.append([target_bw])

    return {
        'inputs': np.array(inputs, dtype=np.float32),
        'targets': np.array(targets, dtype=np.float32),
    }


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Convert NeuStack CSV to training datasets for all 3 AI models'
    )
    parser.add_argument('--data-dir', default='collected_data/',
                        help='Directory containing collected CSV files')
    parser.add_argument('--tcp-samples', help='Override: specific tcp_samples file/dir')
    parser.add_argument('--global-metrics', help='Override: specific global_metrics file/dir')
    parser.add_argument('--output-dir', default='training/real_data/',
                        help='Output directory for datasets')
    parser.add_argument('--model', choices=['all', 'orca', 'anomaly', 'bandwidth'],
                        default='all', help='Which model dataset to generate')
    parser.add_argument('--min-samples', type=int, default=100,
                        help='Minimum samples required')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    # ─── Orca + Bandwidth: 场景 1-4 的 tcp_samples ───
    if args.model in ['all', 'orca', 'bandwidth']:
        print("=" * 60)
        print("[*] Loading TCP samples (scenarios 1-4)...")

        if args.tcp_samples:
            if os.path.isdir(args.tcp_samples):
                tcp_files = sorted(glob.glob(os.path.join(args.tcp_samples, 'tcp_samples*.csv')))
            else:
                tcp_files = [args.tcp_samples]
        else:
            tcp_files = find_files_by_patterns(args.data_dir, ORCA_BW_PATTERNS)

        if not tcp_files:
            print("  ERROR: No tcp_samples files found")
            print(f"  Looked in: {args.data_dir}")
            print(f"  Patterns: {ORCA_BW_PATTERNS}")
            return

        tcp_files.sort()
        samples = merge_tcp_samples(*tcp_files)

        if len(samples) < args.min_samples:
            print(f"  ERROR: Need at least {args.min_samples} samples, got {len(samples)}")
            return

        est_bw_list = estimate_bandwidth(samples)
        min_rtt_list = compute_windowed_min_rtt(samples)

        # 诊断信息
        raw_min_rtts = [s['min_rtt_us'] for s in samples]
        print(f"\n  Diagnostics:")
        print(f"    Raw min_rtt_us:    min={min(raw_min_rtts):.0f}, "
              f"median={np.median(raw_min_rtts):.0f}, max={max(raw_min_rtts):.0f}")
        print(f"    Windowed min_rtt:  min={min(min_rtt_list):.0f}, "
              f"median={np.median(min_rtt_list):.0f}, max={max(min_rtt_list):.0f}")
        print(f"    Est bandwidth:     min={min(est_bw_list):.0f}, "
              f"median={np.median(est_bw_list):.0f}, max={max(est_bw_list):.0f}")

        # Orca 数据集
        if args.model in ['all', 'orca']:
            print("\n" + "-" * 60)
            print("[1/3] Generating Orca dataset...")
            orca_data = generate_orca_dataset(samples, est_bw_list, min_rtt_list)
            orca_path = os.path.join(args.output_dir, 'orca_dataset.npz')
            np.savez(orca_path, **orca_data)
            print(f"  Saved: {orca_path}")
            print(f"    Transitions: {len(orca_data['states'])}")
            print(f"    State dim:   {orca_data['states'].shape[1]}")
            print(f"    Reward range: [{orca_data['rewards'].min():.3f}, "
                  f"{orca_data['rewards'].max():.3f}]")
            for i, name in enumerate(['throughput', 'queuing_delay', 'rtt_ratio',
                                       'loss_rate', 'cwnd_norm', 'inflight_ratio']):
                col = orca_data['states'][:, i]
                print(f"    {name:16s}: [{col.min():.3f}, {col.max():.3f}] "
                      f"mean={col.mean():.3f}")

        # 带宽预测数据集
        if args.model in ['all', 'bandwidth']:
            print("\n" + "-" * 60)
            print("[2/3] Generating Bandwidth Prediction dataset...")
            bw_data = generate_bandwidth_dataset(samples, est_bw_list, min_rtt_list)
            bw_path = os.path.join(args.output_dir, 'bandwidth_dataset.npz')
            np.savez(bw_path, **bw_data)
            print(f"  Saved: {bw_path}")
            print(f"    Sequences:    {len(bw_data['inputs'])}")
            print(f"    Input shape:  {bw_data['inputs'].shape}")
            print(f"    Target range: [{bw_data['targets'].min():.3f}, "
                  f"{bw_data['targets'].max():.3f}]")

    # ─── Anomaly: 全部 6 组的 global_metrics ───
    if args.model in ['all', 'anomaly']:
        print("\n" + "-" * 60)
        print("[3/3] Generating Anomaly Detection dataset...")

        if args.global_metrics:
            if os.path.isdir(args.global_metrics):
                gm_files = sorted(glob.glob(os.path.join(args.global_metrics, 'global_metrics*.csv')))
            else:
                gm_files = [args.global_metrics]
        else:
            gm_files = find_files_by_patterns(args.data_dir, ANOMALY_PATTERNS)

        if not gm_files:
            print("  WARNING: No global_metrics files found, skipping anomaly dataset")
        else:
            gm_files.sort()
            metrics = merge_global_metrics(*gm_files)

            if len(metrics) < 10:
                print("  WARNING: Too few metrics samples for anomaly detection")
            else:
                anomaly_data = generate_anomaly_dataset(metrics)
                anomaly_path = os.path.join(args.output_dir, 'anomaly_dataset.npz')
                np.savez(anomaly_path, **anomaly_data)
                print(f"  Saved: {anomaly_path}")
                print(f"    Samples:     {len(anomaly_data['inputs'])}")
                print(f"    Feature dim: {anomaly_data['inputs'].shape[1]}")
                for i, name in enumerate(['pkt_rx', 'pkt_tx', 'bytes_tx',
                                           'syn_rate', 'rst_rate', 'conn_rate',
                                           'tx_rx_ratio', 'active_conns']):
                    col = anomaly_data['inputs'][:, i]
                    print(f"    {name:12s}: [{col.min():.4f}, {col.max():.4f}] "
                          f"mean={col.mean():.4f} nonzero={np.count_nonzero(col)}")

    print("\n" + "=" * 60)
    print("Dataset generation complete!")
    print(f"Output directory: {args.output_dir}")
    print("=" * 60)


if __name__ == '__main__':
    main()
