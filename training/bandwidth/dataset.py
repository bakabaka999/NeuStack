import numpy as np
import pandas as pd
import torch
from torch.utils.data import Dataset, DataLoader
from typing import Tuple

class SyntheticBandwidthDataset(Dataset):
    """合成带宽预测数据集"""

    def __init__(
        self,
        n_samples: int = 10000,
        history_length: int = 10,
        seed: int = 42
    ):
        """
        Args:
            n_samples: 样本数量
            history_length: 历史序列长度
            seed: 随机种子
        """
        np.random.seed(seed)

        self.history_length = history_length
        self.sequences = []
        self.targets = []

        # 生成多段模拟网络状况
        for _ in range(n_samples // 100):
            seq = self._generate_sequence(100 + history_length)
            for i in range(100):
                # 输入: 过去 history_length 个时间步
                hist = seq[i:i + history_length]
                # 目标: 下一时间步的吞吐量
                target = seq[i + history_length, 0]  # throughput

                self.sequences.append(hist)
                self.targets.append(target)

        self.sequences = np.array(self.sequences, dtype=np.float32)
        self.targets = np.array(self.targets, dtype=np.float32)

    def _generate_sequence(self, length: int) -> np.ndarray:
        """生成一段模拟的网络状况序列"""
        t = np.arange(length)

        # 基础带宽 (带周期性波动)
        base_bw = 0.5 + 0.2 * np.sin(2 * np.pi * t / 50)

        # 随机事件: 突发拥塞
        congestion = np.zeros(length)
        for _ in range(length // 30):
            start = np.random.randint(0, length - 10)
            duration = np.random.randint(5, 15)
            severity = np.random.uniform(0.3, 0.7)
            congestion[start:start + duration] = severity

        # 吞吐量 = 基础 - 拥塞 + 噪声
        throughput = np.clip(
            base_bw - congestion + np.random.normal(0, 0.05, length),
            0.1, 1.0
        )

        # RTT 与拥塞正相关
        rtt = np.clip(
            0.2 + 0.5 * congestion + np.random.normal(0, 0.03, length),
            0.1, 1.0
        )

        # 丢包率在拥塞时升高
        loss = np.clip(
            0.01 + 0.3 * congestion + np.random.normal(0, 0.01, length),
            0.0, 0.5
        )

        return np.column_stack([throughput, rtt, loss])

    def __len__(self) -> int:
        return len(self.sequences)

    def __getitem__(self, idx: int) -> Tuple[torch.Tensor, torch.Tensor]:
        return (
            torch.from_numpy(self.sequences[idx]),
            torch.tensor(self.targets[idx])
        )


def create_dataloaders(
    train_samples: int = 8000,
    val_samples: int = 2000,
    history_length: int = 10,
    batch_size: int = 64,
    seed: int = 42
) -> Tuple[DataLoader, DataLoader]:
    """创建训练和验证数据加载器"""
    train_dataset = SyntheticBandwidthDataset(
        n_samples=train_samples,
        history_length=history_length,
        seed=seed
    )

    val_dataset = SyntheticBandwidthDataset(
        n_samples=val_samples,
        history_length=history_length,
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


class RealBandwidthDataset(Dataset):
    """从 TCPSample 导出的真实数据"""

    def __init__(self, csv_path: str, history_length: int = 10):
        df = pd.read_csv(csv_path)

        # 归一化参数
        MAX_THROUGHPUT = 100e6  # 100 Mbps
        MAX_RTT = 100000        # 100ms in us

        # 提取特征
        throughput = (df['delivery_rate'] / MAX_THROUGHPUT).clip(0, 1).values
        rtt = (df['rtt_us'] / MAX_RTT).clip(0, 1).values

        # 计算丢包率
        loss = np.where(
            df['packets_sent'] > 0,
            df['packets_lost'] / df['packets_sent'],
            0
        ).clip(0, 1)

        # 组合成序列
        features = np.column_stack([throughput, rtt, loss]).astype(np.float32)

        # 构建样本
        self.sequences = []
        self.targets = []

        for i in range(len(features) - history_length):
            self.sequences.append(features[i:i + history_length])
            self.targets.append(features[i + history_length, 0])  # 下一步的吞吐量

        self.sequences = np.array(self.sequences)
        self.targets = np.array(self.targets, dtype=np.float32)

    def __len__(self):
        return len(self.sequences)

    def __getitem__(self, idx):
        return (
            torch.from_numpy(self.sequences[idx]),
            torch.tensor(self.targets[idx])
        )


class NpzBandwidthDataset(Dataset):
    """从 npz 文件加载的带宽预测数据集"""

    def __init__(self, inputs: np.ndarray, targets: np.ndarray):
        """
        Args:
            inputs: 形状 (N, history_length, 3) 的序列数据
            targets: 形状 (N, 1) 或 (N,) 的目标值
        """
        self.sequences = inputs.astype(np.float32)
        self.targets = targets.flatten().astype(np.float32)

    def __len__(self):
        return len(self.sequences)

    def __getitem__(self, idx):
        return (
            torch.from_numpy(self.sequences[idx]),
            torch.tensor(self.targets[idx])
        )


def create_dataloaders_from_npz(
    npz_path: str,
    train_ratio: float = 0.8,
    batch_size: int = 64,
    seed: int = 42
) -> Tuple[DataLoader, DataLoader]:
    """从 npz 文件创建训练和验证数据加载器"""
    data = np.load(npz_path)
    inputs = data['inputs']
    targets = data['targets']

    # 打乱并分割数据
    np.random.seed(seed)
    indices = np.random.permutation(len(inputs))
    split = int(len(inputs) * train_ratio)

    train_indices = indices[:split]
    val_indices = indices[split:]

    train_loader = DataLoader(
        NpzBandwidthDataset(inputs[train_indices], targets[train_indices]),
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True
    )

    val_loader = DataLoader(
        NpzBandwidthDataset(inputs[val_indices], targets[val_indices]),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True
    )

    return train_loader, val_loader
