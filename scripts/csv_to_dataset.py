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
import glob
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

def find_csv_files(path: str, pattern: str) -> List[str]:
    """如果 path 是目录，找所有匹配 pattern 的 CSV；否则返回单个文件"""
    if os.path.isdir(path):
        files = glob.glob(os.path.join(path, pattern))
        files.sort()
        if not files:
            raise FileNotFoundError(f"No {pattern} files found in {path}")
        print(f"  Found {len(files)} files in {path}")
        return files
    elif os.path.isfile(path):
        print(f"  Using single file: {path}")
        return [path]
    else:
        raise FileNotFoundError(f"Path not found: {path}")


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
                except (ValueError, KeyError) as e:
                    continue
        all_samples.extend(samples)
        print(f"    Loaded {len(samples)} samples from {os.path.basename(csv_file)}")

    print(f"  Total: {len(all_samples)} samples merged")
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
        print(f"    Loaded {len(metrics)} metrics from {os.path.basename(csv_file)}")

    print(f"  Total: {len(all_metrics)} metrics merged")
    return all_metrics


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

    原始 min_rtt_us 的问题：TUN 设备的 SYN/SYN-ACK 是本地处理 (~18us)，
    但数据传输时 RTT 是真实网络延迟 (~6000us)，导致 min_rtt 被污染。

    解决方案：使用 srtt_us 的滑动窗口 P10 分位数作为 min_rtt 基线。
    - 使用 percentile 而非 min，可以过滤掉异常低的 RTT 样本
    - 窗口大小 100 ≈ 10 秒 (采样间隔 ~100ms)
    - P10 比 min 更鲁棒，但仍能捕捉到最低延迟

    参数:
        window: 滑动窗口大小
        percentile: 使用的分位数 (0=min, 50=median, 100=max)
    """
    windowed = []
    for i, s in enumerate(samples):
        start = max(0, i - window)
        # 使用 srtt (平滑 RTT) 而非瞬时 rtt，更稳定
        window_srtts = [
            samples[j]['srtt_us']
            for j in range(start, i + 1)
            if samples[j]['srtt_us'] > 0
        ]
        if window_srtts:
            # 使用 percentile 而非 min，过滤异常低值
            base_rtt = np.percentile(window_srtts, percentile)
            windowed.append(max(base_rtt, 1))
        else:
            windowed.append(max(s['rtt_us'], 1))
    return windowed


# ============================================================================
# Orca 数据集生成 (DDPG: state, action, reward, next_state, done)
# ============================================================================

def compute_orca_features(sample: Dict, est_bw: float, min_rtt: float) -> np.ndarray:
    """
    从 TCPSample 提取 6 维 OrcaFeatures

    参数:
        sample: TCP 样本
        est_bw: 估计带宽
        min_rtt: 窗口化 min_rtt (而非原始 min_rtt_us)
    """
    rtt = sample['rtt_us']
    delivery_rate = sample['delivery_rate']
    cwnd = max(sample['cwnd'], 1)
    packets_sent = max(sample['packets_sent'], 1)

    # BDP (Bandwidth-Delay Product) in MSS units
    bdp = (est_bw * min_rtt / 1e6) / MSS if min_rtt > 0 else 1
    bdp = max(bdp, 1)

    # 计算特征并 clip 到合理范围
    throughput_norm = np.clip(delivery_rate / max(est_bw, 1), 0, 2)
    queuing_delay_norm = np.clip((rtt - min_rtt) / max(min_rtt, 1), 0, 5)
    rtt_ratio = np.clip(rtt / max(min_rtt, 1), 1, 5)
    loss_rate = np.clip(sample['packets_lost'] / packets_sent, 0, 1)
    cwnd_norm = np.clip(cwnd / max(bdp, 1), 0, 10)
    in_flight_ratio = np.clip(sample['bytes_in_flight'] / max(cwnd * MSS, 1), 0, 2)

    return np.array([
        throughput_norm,
        queuing_delay_norm,
        rtt_ratio,
        loss_rate,
        cwnd_norm,
        in_flight_ratio,
    ], dtype=np.float32)


def compute_orca_reward(sample: Dict, est_bw: float, min_rtt: float) -> float:
    """
    Orca 奖励函数: 吞吐 - 延迟惩罚 - 丢包惩罚

    使用窗口化 min_rtt 而非原始 min_rtt_us

    调整后的惩罚系数:
    - 原始: delay_penalty = 2.0 * delay_ratio, loss_penalty = 10.0 * loss_rate
    - 问题: 真实网络丢包率 26%，导致几乎所有 reward < 0
    - 解决: 降低惩罚系数，使用 sqrt 缓解极端值，让 reward 有正有负
    """
    rtt = sample['rtt_us']
    queuing_delay = max(0, rtt - min_rtt)

    # 吞吐奖励 (0~1)
    throughput_reward = sample['delivery_rate'] / max(est_bw, 1)

    # 延迟惩罚: 使用 sqrt 缓解，系数从 2.0 降到 0.5
    queuing_delay_ratio = np.clip(queuing_delay / max(min_rtt, 1), 0, 5)
    delay_penalty = 0.5 * np.sqrt(queuing_delay_ratio)  # sqrt: 5 -> 1.1

    # 丢包惩罚: 系数从 10.0 降到 2.0
    # 26% 丢包: 10*0.26=2.6 -> 2*0.26=0.52
    loss_rate = sample['packets_lost'] / max(sample['packets_sent'], 1)
    loss_penalty = 2.0 * loss_rate

    reward = throughput_reward - delay_penalty - loss_penalty

    # Clip reward 到合理范围 (-2 到 1)
    return np.clip(reward, -2, 1)


def generate_orca_dataset(samples: List[Dict], est_bw_list: List[float], min_rtt_list: List[float]) -> Dict:
    """生成 Orca DDPG 训练数据"""
    states, actions, rewards, next_states, dones = [], [], [], [], []

    for i in range(len(samples) - 1):
        s = samples[i]
        s_next = samples[i + 1]
        est_bw = est_bw_list[i]
        min_rtt = min_rtt_list[i]

        state = compute_orca_features(s, est_bw, min_rtt)
        next_state = compute_orca_features(s_next, est_bw_list[i + 1], min_rtt_list[i + 1])
        reward = compute_orca_reward(s, est_bw, min_rtt)

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
    # 计算 delta (确保非负)
    delta_syn = max(0, current.get('syn_received', 0) - prev.get('syn_received', 0))
    delta_rst = max(0, current.get('rst_received', 0) - prev.get('rst_received', 0))
    delta_conn = max(0, current.get('conn_established', 0) - prev.get('conn_established', 0))
    delta_packets = max(0, current.get('packets_rx', 0) - prev.get('packets_rx', 0))
    delta_bytes = max(0, current.get('bytes_rx', 0) - prev.get('bytes_rx', 0))

    # 归一化为速率
    syn_rate = delta_syn / interval_sec
    rst_rate = delta_rst / interval_sec
    new_conn_rate = delta_conn / interval_sec
    packet_rate = delta_packets / interval_sec
    avg_packet_size = delta_bytes / max(delta_packets, 1)

    # 归一化到 [0, 1] 范围 (用 np.clip 确保上下界)
    return np.array([
        np.clip(syn_rate / 1000, 0, 1),           # 归一化: 0-1000 SYN/s
        np.clip(rst_rate / 100, 0, 1),            # 归一化: 0-100 RST/s
        np.clip(new_conn_rate / 500, 0, 1),       # 归一化: 0-500 conn/s
        np.clip(packet_rate / 100000, 0, 1),      # 归一化: 0-100K pkt/s
        np.clip(avg_packet_size / 1500, 0, 1),    # 归一化: 0-MTU
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
    min_rtt_list: List[float],
    history_len: int = BANDWIDTH_HISTORY_LEN
) -> Dict:
    """
    生成带宽预测 LSTM 训练数据

    输入: 过去 N 个时间步的 (throughput, rtt_ratio, loss_rate)
    输出: 下一时刻的带宽 (归一化)

    与 C++ ai_features.hpp 中的 BandwidthFeatures 保持一致

    参数:
        min_rtt_list: 窗口化 min_rtt (而非原始 min_rtt_us)
    """
    inputs = []   # [batch, history_len, 3]
    targets = []  # [batch, 1]

    for i in range(history_len, len(samples) - 1):
        # 提取历史窗口
        history = []
        for j in range(i - history_len, i):
            s = samples[j]
            min_rtt = max(min_rtt_list[j], 1)
            packets_sent = max(s['packets_sent'], 1)

            throughput_norm = np.clip(s['delivery_rate'] / MAX_BW, 0, 1)
            rtt_ratio = np.clip(s['rtt_us'] / min_rtt, 1, 5)
            loss_rate = np.clip(s['packets_lost'] / packets_sent, 0, 1)

            history.append([throughput_norm, rtt_ratio, loss_rate])

        # 目标: 下一时刻的带宽 (归一化)
        target_bw = np.clip(samples[i]['delivery_rate'] / MAX_BW, 0, 1)

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

        print("[*] Loading TCP samples...")
        try:
            csv_files = find_csv_files(args.tcp_samples, 'tcp_samples*.csv')
            samples = merge_tcp_samples(*csv_files)
        except FileNotFoundError as e:
            print(f"ERROR: {e}")
            return

        if len(samples) < args.min_samples:
            print(f"ERROR: Need at least {args.min_samples} samples")
            return

        est_bw_list = estimate_bandwidth(samples)
        min_rtt_list = compute_windowed_min_rtt(samples)

        # 打印诊断信息
        raw_min_rtts = [s['min_rtt_us'] for s in samples]
        print(f"\n  Raw min_rtt_us:      min={min(raw_min_rtts):.0f}, "
              f"median={np.median(raw_min_rtts):.0f}, max={max(raw_min_rtts):.0f}")
        print(f"  Windowed min_rtt:    min={min(min_rtt_list):.0f}, "
              f"median={np.median(min_rtt_list):.0f}, max={max(min_rtt_list):.0f}")
        print(f"  Est bandwidth:       min={min(est_bw_list):.0f}, "
              f"median={np.median(est_bw_list):.0f}, max={max(est_bw_list):.0f}")

        # Orca 数据集
        if args.model in ['all', 'orca']:
            print("\n[1/3] Generating Orca dataset...")
            orca_data = generate_orca_dataset(samples, est_bw_list, min_rtt_list)
            orca_path = os.path.join(args.output_dir, 'orca_dataset.npz')
            np.savez(orca_path, **orca_data)
            print(f"  Saved: {orca_path}")
            print(f"    Transitions: {len(orca_data['states'])}")
            print(f"    State dim:   {orca_data['states'].shape[1]}")
            print(f"    Reward range: [{orca_data['rewards'].min():.3f}, "
                  f"{orca_data['rewards'].max():.3f}]")
            # 打印特征统计
            for i, name in enumerate(['throughput', 'queuing_delay', 'rtt_ratio',
                                       'loss_rate', 'cwnd_norm', 'inflight_ratio']):
                col = orca_data['states'][:, i]
                print(f"    {name:16s}: [{col.min():.3f}, {col.max():.3f}] "
                      f"mean={col.mean():.3f}")

        # 带宽预测数据集
        if args.model in ['all', 'bandwidth']:
            print("\n[2/3] Generating Bandwidth Prediction dataset...")
            bw_data = generate_bandwidth_dataset(samples, est_bw_list, min_rtt_list)
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
            try:
                csv_files = find_csv_files(args.global_metrics, 'global_metrics*.csv')
                metrics = merge_global_metrics(*csv_files)
            except FileNotFoundError as e:
                print(f"WARNING: {e}, skipping anomaly dataset")
                metrics = []

            if len(metrics) < 10:
                print("  WARNING: Too few metrics samples for anomaly detection")
            elif metrics:
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

    