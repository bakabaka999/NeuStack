import numpy as np
import pandas as pd
import torch
from torch.utils.data import Dataset, DataLoader
from typing import Tuple, Optional

class SyntheticTrafficDataset(Dataset):
    """合成网络流量数据集"""

    def __init__(self, n_sample: int = 10000, anomaly_ratio: float = 0.0, seed: int = 43):
        """
        Args:
            n_samples: 样本数量
            anomaly_ratio: 异常样本比例 (训练时设为 0)
            seed: 随机种子
        """
        np.random.seed(seed)

        n_normal = int(n_sample * (1 - anomaly_ratio))
        n_anomaly = n_sample - n_normal

        # 生成正常流量
        normal_data = self._generate_normal(n_normal)

        # 生成异常流量 (仅用于验证)
        if n_anomaly > 0:
            anomaly_data = self._generate_anomaly(n_anomaly)
            self.data = np.vstack([normal_data, anomaly_data])
            self.labels = np.array([0] * n_normal + [1] * n_anomaly)
        else:
            self.data = normal_data
            self.labels = np.zeros(n_normal)

        # 转换为 float32
        self.data = self.data.astype(np.float32)

    def _generate_normal(self, n: int) -> np.ndarray:
        """生成正常流量特征"""
        return np.column_stack([
            np.random.normal(0.10, 0.03, n),  # syn_rate: 均值 0.1, 标准差 0.03
            np.random.normal(0.05, 0.02, n),  # rst_rate: 均值 0.05
            np.random.normal(0.15, 0.05, n),  # new_conn_rate: 均值 0.15
            np.random.normal(0.30, 0.10, n),  # packet_rate: 均值 0.3
            np.random.normal(0.50, 0.10, n),  # avg_packet_size: 均值 0.5 (归一化后)
        ]).clip(0, 1)  # 确保在 [0, 1] 范围
    
    def _generate_anomaly(self, n: int) -> np.ndarray:
        """生成异常流量特征 (混合多种攻击类型)"""
        n_syn_flood = n // 3
        n_port_scan = n // 3
        n_ddos = n - n_syn_flood - n_port_scan

        # SYN Flood: syn_rate 异常高
        syn_flood = np.column_stack([
            np.random.normal(0.85, 0.05, n_syn_flood),  # syn_rate 很高
            np.random.normal(0.05, 0.02, n_syn_flood),  # rst_rate 正常
            np.random.normal(0.80, 0.10, n_syn_flood),  # new_conn_rate 很高
            np.random.normal(0.70, 0.15, n_syn_flood),  # packet_rate 较高
            np.random.normal(0.30, 0.05, n_syn_flood),  # avg_packet_size 较小 (SYN 包小)
        ])

        # 端口扫描: rst_rate 异常高
        port_scan = np.column_stack([
            np.random.normal(0.60, 0.10, n_port_scan),  # syn_rate 较高
            np.random.normal(0.70, 0.10, n_port_scan),  # rst_rate 很高 (被拒绝)
            np.random.normal(0.55, 0.10, n_port_scan),  # new_conn_rate 较高
            np.random.normal(0.50, 0.15, n_port_scan),  # packet_rate 中等
            np.random.normal(0.25, 0.05, n_port_scan),  # avg_packet_size 较小
        ])

        # DDoS: packet_rate 异常高
        ddos = np.column_stack([
            np.random.normal(0.20, 0.05, n_ddos),       # syn_rate 略高
            np.random.normal(0.10, 0.03, n_ddos),       # rst_rate 略高
            np.random.normal(0.25, 0.08, n_ddos),       # new_conn_rate 略高
            np.random.normal(0.95, 0.03, n_ddos),       # packet_rate 极高
            np.random.normal(0.60, 0.15, n_ddos),       # avg_packet_size 可变
        ])

        return np.vstack([syn_flood, port_scan, ddos]).clip(0, 1)
    
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
    """创建训练和验证数据加载器"""

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


class RealTrafficDataset(Dataset):
    """从 NeuStack 导出的真实流量数据"""

    def __init__(self, csv_path: str, interval_ms: float = 100.0):
        df = pd.read_csv(csv_path)

        # 计算速率 (每秒)
        scale = 1000.0 / interval_ms

        # 归一化参数 (需要根据实际流量调整)
        MAX_SYN_RATE = 1000.0
        MAX_RST_RATE = 500.0
        MAX_CONN_RATE = 200.0
        MAX_PACKET_RATE = 10000.0
        MAX_PACKET_SIZE = 1500.0

        # 计算特征 (与 Delta 字段对应)
        self.data = np.column_stack([
            (df['syn_received'] * scale / MAX_SYN_RATE).clip(0, 1),
            (df['rst_received'] * scale / MAX_RST_RATE).clip(0, 1),
            (df['conn_established'] * scale / MAX_CONN_RATE).clip(0, 1),
            (df['packets_rx'] * scale / MAX_PACKET_RATE).clip(0, 1),
            np.where(df['packets_rx'] > 0,
                     df['bytes_rx'] / df['packets_rx'] / MAX_PACKET_SIZE,
                     0.5).clip(0, 1),
        ]).astype(np.float32)

        # 真实数据默认标签为 0 (正常)，需要人工标注异常
        self.labels = np.zeros(len(self.data))

    def __len__(self):
        return len(self.data)

    def __getitem__(self, idx):
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


class NpzDataset(Dataset):
    """从 npz 文件加载的数据集 (真实采集数据)"""

    def __init__(self, npz_path: str, labels: Optional[np.ndarray] = None):
        """
        Args:
            npz_path: npz 文件路径，需包含 'inputs' 键
            labels: 可选的标签数组，默认全为 0 (正常)
        """
        data = np.load(npz_path)
        self.data = data['inputs'].astype(np.float32)

        if labels is not None:
            self.labels = labels
        else:
            # 真实数据默认标签为 0 (正常)
            self.labels = np.zeros(len(self.data))

    def __len__(self):
        return len(self.data)

    def __getitem__(self, idx):
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


def create_dataloaders_from_npz(
    npz_path: str,
    train_ratio: float = 0.8,
    batch_size: int = 64,
    seed: int = 42
) -> Tuple[DataLoader, DataLoader]:
    """从 npz 文件创建训练和验证数据加载器"""
    data = np.load(npz_path)
    inputs = data['inputs'].astype(np.float32)

    # 打乱并分割数据
    np.random.seed(seed)
    indices = np.random.permutation(len(inputs))
    split = int(len(inputs) * train_ratio)

    train_indices = indices[:split]
    val_indices = indices[split:]

    # 训练集：正常数据 (标签 0)
    train_data = inputs[train_indices]
    train_labels = np.zeros(len(train_data))

    # 验证集：正常数据 (标签 0)
    # 注意：真实数据没有异常标签，评估时只能看重构误差分布
    val_data = inputs[val_indices]
    val_labels = np.zeros(len(val_data))

    class SimpleDataset(Dataset):
        def __init__(self, data, labels):
            self.data = data
            self.labels = labels

        def __len__(self):
            return len(self.data)

        def __getitem__(self, idx):
            return torch.from_numpy(self.data[idx]), int(self.labels[idx])

    train_loader = DataLoader(
        SimpleDataset(train_data, train_labels),
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True
    )

    val_loader = DataLoader(
        SimpleDataset(val_data, val_labels),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True
    )

    return train_loader, val_loader
