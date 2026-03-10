import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader
from typing import Tuple, Optional


class SyntheticTrafficDataset(Dataset):
    """
    合成网络流量数据集 (8 维，匹配 C++ AnomalyFeatures::from_delta)

    Ratio-based volume-invariant features:
      0. log_pkt_rate      log1p(pkt_rx+pkt_tx) / log1p(40000)
      1. bytes_per_pkt     bytes_tx / max(pkt_tx,1) / 1500
      2. syn_ratio         syn_received / max(pkt_rx,1)
      3. rst_ratio         rst_received / max(pkt_rx,1)
      4. conn_completion   conn_est / max(syn,1); syn=0 → 1.0
      5. tx_rx_ratio       pkt_tx / max(pkt_rx,1) / 2
      6. log_active_conn   log1p(active) / log1p(1000)
      7. log_conn_reset  log1p(conn_reset) / log1p(100)
    """

    def __init__(self, n_sample: int = 10000, anomaly_ratio: float = 0.0, seed: int = 43):
        np.random.seed(seed)

        n_normal = int(n_sample * (1 - anomaly_ratio))
        n_anomaly = n_sample - n_normal

        normal_data = self._generate_normal(n_normal)

        if n_anomaly > 0:
            anomaly_data = self._generate_anomaly(n_anomaly)
            self.data = np.vstack([normal_data, anomaly_data])
            self.labels = np.array([0] * n_normal + [1] * n_anomaly)
        else:
            self.data = normal_data
            self.labels = np.zeros(n_normal)

        self.data = self.data.astype(np.float32)

    def _generate_normal(self, n: int) -> np.ndarray:
        """生成正常流量特征 (8 维, ratio-based)"""
        return np.column_stack([
            np.random.normal(0.5, 0.25, n),                              # log_pkt_rate: moderate activity
            np.random.normal(0.6, 0.2, n),                               # bytes_per_pkt: typical ~900B packets
            np.random.exponential(0.01, n),                               # syn_ratio: rare SYN in normal
            np.random.exponential(0.005, n),                              # rst_ratio: very rare RST
            np.random.beta(8, 2, n),                                      # conn_completion: peaked near 1.0
            np.random.normal(0.45, 0.15, n),                              # tx_rx_ratio: roughly balanced
            np.random.normal(0.35, 0.2, n),                               # log_active_conn: moderate connections
            np.random.exponential(0.03, n),                               # log_conn_reset: rare resets (log-compressed)
        ]).clip(0, 1)

    def _generate_anomaly(self, n: int) -> np.ndarray:
        """生成异常流量特征 (8 维, 混合多种异常模式)"""
        k = n // 3
        rest = n - 2 * k

        # SYN flood: high syn_ratio, low conn_completion
        syn_flood = np.column_stack([
            np.random.normal(0.5, 0.2, k),       # log_pkt_rate: moderate-high
            np.random.normal(0.03, 0.01, k),      # bytes_per_pkt: small SYN packets ~40B
            np.random.uniform(0.3, 0.7, k),       # syn_ratio: high!
            np.random.exponential(0.01, k),        # rst_ratio: normal
            np.random.uniform(0.01, 0.1, k),       # conn_completion: very low!
            np.random.normal(0.2, 0.1, k),         # tx_rx_ratio: low (mostly receiving SYNs)
            np.random.normal(0.6, 0.15, k),        # log_active_conn: elevated
            np.random.exponential(0.03, k),         # log_conn_reset: normal
        ])

        # Port scan: high rst_ratio, moderate syn_ratio
        port_scan = np.column_stack([
            np.random.normal(0.4, 0.15, k),       # log_pkt_rate: moderate
            np.random.normal(0.03, 0.01, k),      # bytes_per_pkt: small scan packets
            np.random.uniform(0.2, 0.5, k),       # syn_ratio: elevated
            np.random.uniform(0.3, 0.7, k),       # rst_ratio: high! (closed ports)
            np.random.uniform(0.05, 0.2, k),       # conn_completion: low
            np.random.normal(0.45, 0.1, k),        # tx_rx_ratio: balanced
            np.random.normal(0.3, 0.15, k),        # log_active_conn: normal
            np.random.exponential(0.03, k),         # log_conn_reset: normal
        ])

        # Connection anomaly: high log_conn_reset, abnormal conn_completion
        conn_anomaly = np.column_stack([
            np.random.normal(0.5, 0.2, rest),     # log_pkt_rate: moderate
            np.random.normal(0.5, 0.2, rest),     # bytes_per_pkt: mixed
            np.random.exponential(0.03, rest),     # syn_ratio: slightly elevated
            np.random.exponential(0.01, rest),     # rst_ratio: normal
            np.random.uniform(0.3, 0.6, rest),     # conn_completion: mediocre
            np.random.normal(0.45, 0.15, rest),    # tx_rx_ratio: balanced
            np.random.normal(0.7, 0.15, rest),     # log_active_conn: high
            np.random.uniform(0.4, 0.8, rest),     # log_conn_reset: high! (many resets)
        ])

        return np.vstack([syn_flood, port_scan, conn_anomaly]).clip(0, 1)

    def __len__(self) -> int:
        return len(self.data)

    def __getitem__(self, idx: int) -> Tuple[torch.Tensor, int]:
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


def create_dataloaders(
    train_samples: int = 8000,
    val_samples: int = 2000,
    batch_size: int = 64,
    seed: int = 42
) -> Tuple[DataLoader, DataLoader]:
    """创建训练和验证数据加载器 (合成数据)"""

    # 训练集：只用正常数据
    train_dataset = SyntheticTrafficDataset(
        n_sample=train_samples,
        anomaly_ratio=0.0,
        seed=seed
    )

    # 验证集：混合正常和异常数据
    val_dataset = SyntheticTrafficDataset(
        n_sample=val_samples,
        anomaly_ratio=0.3,
        seed=seed + 1
    )

    train_loader = DataLoader(
        train_dataset,
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True
    )

    val_loader = DataLoader(
        val_dataset,
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True
    )

    return train_loader, val_loader


class NpzDataset(Dataset):
    """从 npz 文件加载的数据集 (真实采集数据)"""

    def __init__(self, data: np.ndarray, labels: np.ndarray):
        self.data = data.astype(np.float32)
        self.labels = labels.astype(np.int64)

    def __len__(self):
        return len(self.data)

    def __getitem__(self, idx):
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


def _augment_normal_data(normal_data: np.ndarray, seed: int = 42, n_aug: int = 300) -> np.ndarray:
    """
    基于真实正常数据分布，增强覆盖不足的特征组合

    真实采集数据缺少某些正常场景（如单连接大文件下载：高流量+少连接），
    导致 autoencoder 将这些正常模式视为 OOD。通过噪声增强填补这些空白。

    Features (matching C++ AnomalyFeatures::from_delta):
      0. log_pkt_rate    1. bytes_per_pkt    2. syn_ratio     3. rst_ratio
      4. conn_completion 5. tx_rx_ratio      6. log_active_conn 7. log_conn_reset
    """
    rng = np.random.RandomState(seed)
    augmented = []

    # 场景 1: 大文件下载 — 高流量 + 少连接 + 有 conn_reset
    # 真实数据中 0 samples 有 log_pkt_rate>0.8 & log_active_conn<0.3
    # 正常下载过程中连接结束会产生 RST/FIN → log_conn_reset 可达 0.15-0.5
    n1 = n_aug // 3
    aug1 = np.column_stack([
        rng.uniform(0.7, 1.0, n1),          # log_pkt_rate: high traffic
        rng.uniform(0.85, 0.98, n1),         # bytes_per_pkt: large data packets
        rng.exponential(0.002, n1),           # syn_ratio: near zero
        rng.exponential(0.002, n1),           # rst_ratio: near zero
        rng.uniform(0.9, 1.0, n1),           # conn_completion: all complete
        rng.uniform(0.4, 0.8, n1),           # tx_rx_ratio: download-biased
        rng.uniform(0.0, 0.35, n1),          # log_active_conn: few connections
        rng.uniform(0.0, 0.45, n1),          # log_conn_reset: 0 to ~5 resets is normal
    ]).clip(0, 1).astype(np.float32)
    augmented.append(aug1)

    # 场景 2: 普通浏览/短连接 — 中等流量 + 各种 conn_reset 水平
    # 正常网页浏览：短连接频繁建立/关闭，log_conn_reset 可达 0.3-0.5
    n2 = n_aug // 3
    aug2 = np.column_stack([
        rng.uniform(0.45, 0.85, n2),        # log_pkt_rate: moderate
        rng.uniform(0.4, 0.95, n2),          # bytes_per_pkt: mixed
        rng.exponential(0.01, n2),            # syn_ratio: normal
        rng.exponential(0.005, n2),           # rst_ratio: normal
        rng.beta(8, 2, n2),                   # conn_completion: mostly complete
        rng.uniform(0.3, 0.7, n2),           # tx_rx_ratio: balanced
        rng.uniform(0.1, 0.6, n2),           # log_active_conn: varied
        rng.uniform(0.0, 0.5, n2),           # log_conn_reset: normal connection teardowns
    ]).clip(0, 1).astype(np.float32)
    augmented.append(aug2)

    # 场景 3: 对真实高流量样本添加噪声，扩展覆盖范围
    high_traffic = normal_data[normal_data[:, 0] > 0.7]
    if len(high_traffic) > 0:
        n3 = n_aug // 3
        indices = rng.choice(len(high_traffic), n3, replace=True)
        noisy = high_traffic[indices].copy()
        # 扩展 log_active_conn 和 log_conn_reset 范围
        noisy[:, 6] = rng.uniform(0.0, 0.5, n3)   # log_active_conn: varied
        noisy[:, 7] = rng.uniform(0.0, 0.4, n3)   # log_conn_reset: varied
        # 添加轻微噪声到其他特征
        noise = rng.normal(0, 0.02, noisy.shape)
        noise[:, 6] = 0  # already adjusted
        noise[:, 7] = 0  # already adjusted
        noisy = (noisy + noise).clip(0, 1).astype(np.float32)
        augmented.append(noisy)

    return np.vstack(augmented) if augmented else np.empty((0, 8), dtype=np.float32)


def create_dataloaders_from_npz(
    npz_path: str,
    train_ratio: float = 0.8,
    batch_size: int = 64,
    seed: int = 42
) -> Tuple[DataLoader, DataLoader]:
    """
    从 npz 文件创建训练和验证数据加载器

    - 训练集: 只用 label=0 的正常数据 (80%) + 增强数据
    - 验证集: label=0 的剩余 20% + 全部 label=1 异常数据
    """
    data = np.load(npz_path)
    inputs = data['inputs'].astype(np.float32)
    labels = data.get('labels', np.zeros(len(inputs), dtype=np.int32))

    normal_mask = labels == 0
    anomaly_mask = labels == 1

    normal_inputs = inputs[normal_mask]
    anomaly_inputs = inputs[anomaly_mask]

    n_normal = len(normal_inputs)
    n_anomaly = len(anomaly_inputs)

    print(f"  Data split:")
    print(f"    Normal samples: {n_normal}")
    print(f"    Anomaly samples: {n_anomaly}")

    # 打乱正常数据并分割
    np.random.seed(seed)
    perm = np.random.permutation(n_normal)
    split = int(n_normal * train_ratio)

    train_data = normal_inputs[perm[:split]]

    # 增强训练数据：填补覆盖不足的正常流量模式
    aug_data = _augment_normal_data(train_data, seed=seed + 100)
    if len(aug_data) > 0:
        print(f"    Augmented normal samples: {len(aug_data)}")
        train_data = np.vstack([train_data, aug_data])

    train_labels = np.zeros(len(train_data), dtype=np.int32)

    # 验证集: 剩余正常 + 全部异常
    val_normal = normal_inputs[perm[split:]]
    if n_anomaly > 0:
        val_data = np.vstack([val_normal, anomaly_inputs])
        val_labels = np.concatenate([
            np.zeros(len(val_normal), dtype=np.int32),
            np.ones(n_anomaly, dtype=np.int32)
        ])
    else:
        val_data = val_normal
        val_labels = np.zeros(len(val_normal), dtype=np.int32)

    print(f"    Train (normal only): {len(train_data)}")
    print(f"    Val (normal={len(val_normal)}, anomaly={n_anomaly}): {len(val_data)}")

    train_loader = DataLoader(
        NpzDataset(train_data, train_labels),
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True
    )

    val_loader = DataLoader(
        NpzDataset(val_data, val_labels),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True
    )

    return train_loader, val_loader
