"""
training/security/dataset.py

安全异常检测数据集

支持两种数据源:
  1. NPZ 文件 (csv_to_dataset.py 生成, 含 inputs + labels)
  2. 合成数据 (无真实数据时的 fallback)

特征维度: 8 (与 ISecurityModel::Input 一致)
  pps_norm, bps_norm, syn_rate_norm, rst_rate_norm,
  syn_ratio_norm, new_conn_rate_norm, avg_pkt_size_norm, rst_ratio_norm
"""

import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader
from typing import Tuple, Optional, Dict


class SecurityDataset(Dataset):
    """通用安全数据集"""

    def __init__(self, data: np.ndarray, labels: np.ndarray):
        self.data = data.astype(np.float32)
        self.labels = labels.astype(np.int64)

    def __len__(self):
        return len(self.data)

    def __getitem__(self, idx) -> Tuple[torch.Tensor, int]:
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


class SyntheticSecurityDataset(Dataset):
    """
    合成安全流量数据集

    8 维特征 (ISecurityModel::Input):
      pps_norm, bps_norm, syn_rate_norm, rst_rate_norm,
      syn_ratio_norm, new_conn_rate_norm, avg_pkt_size_norm, rst_ratio_norm
    """

    def __init__(
        self,
        n_samples: int = 10000,
        anomaly_ratio: float = 0.0,
        seed: int = 42,
    ):
        np.random.seed(seed)

        n_normal = int(n_samples * (1 - anomaly_ratio))
        n_anomaly = n_samples - n_normal

        normal_data = self._generate_normal(n_normal)

        if n_anomaly > 0:
            anomaly_data = self._generate_anomaly(n_anomaly)
            self.data = np.vstack([normal_data, anomaly_data]).astype(np.float32)
            self.labels = np.array([0] * n_normal + [1] * n_anomaly, dtype=np.int64)
        else:
            self.data = normal_data.astype(np.float32)
            self.labels = np.zeros(n_normal, dtype=np.int64)

    def _generate_normal(self, n: int) -> np.ndarray:
        """
        正常流量特征分布

        基于 SecurityMetrics 在正常网络活动下的期望值:
        - pps: 低~中 (少量包)
        - bps: 与 pps 正相关
        - syn/rst: 很低 (正常连接建立)
        - syn_ratio: ~1.0 (SYN ≈ SYN-ACK), sigmoid(1.0, midpoint=5) ≈ 0.018
        - new_conn_rate: 很低
        - avg_pkt_size: 中等 (混合大小包)
        - rst_ratio: ~0 (几乎没有 RST)
        """
        return np.column_stack([
            np.random.lognormal(-3.0, 0.8, n).clip(0, 1),  # pps_norm: 偏左分布
            np.random.lognormal(-3.5, 1.0, n).clip(0, 1),  # bps_norm: log 归一化后偏左
            np.random.exponential(0.01, n).clip(0, 1),      # syn_rate_norm: 极低
            np.random.exponential(0.005, n).clip(0, 1),     # rst_rate_norm: 极低
            np.random.normal(0.018, 0.008, n).clip(0, 1),   # syn_ratio_norm: sigmoid(~1)
            np.random.exponential(0.01, n).clip(0, 1),      # new_conn_rate_norm: 极低
            np.random.normal(0.5, 0.15, n).clip(0, 1),      # avg_pkt_size_norm: 中等
            np.random.exponential(0.005, n).clip(0, 1),      # rst_ratio_norm: ~0
        ])

    def _generate_anomaly(self, n: int) -> np.ndarray:
        """生成多种攻击类型的异常数据"""
        n_per_type = max(1, n // 5)
        anomalies = []

        # 1. SYN Flood: 高 SYN, 高 syn_ratio, 高 new_conn_rate
        syn_flood = np.column_stack([
            np.random.uniform(0.3, 0.7, n_per_type),    # pps: 中高
            np.random.uniform(0.1, 0.3, n_per_type),    # bps: 中 (SYN 小包)
            np.random.uniform(0.5, 1.0, n_per_type),    # syn_rate: 很高
            np.random.uniform(0.0, 0.05, n_per_type),   # rst_rate: 低
            np.random.uniform(0.7, 1.0, n_per_type),    # syn_ratio: 极高 (SYN >> SYN-ACK)
            np.random.uniform(0.5, 1.0, n_per_type),    # new_conn_rate: 很高
            np.random.uniform(0.02, 0.08, n_per_type),  # avg_pkt_size: 小 (SYN 包)
            np.random.uniform(0.0, 0.03, n_per_type),   # rst_ratio: 低
        ])
        anomalies.append(syn_flood)

        # 2. Port Scan: 高 SYN, 高 RST, 高 rst_ratio
        port_scan = np.column_stack([
            np.random.uniform(0.2, 0.5, n_per_type),    # pps: 中
            np.random.uniform(0.05, 0.15, n_per_type),  # bps: 低中
            np.random.uniform(0.3, 0.7, n_per_type),    # syn_rate: 高
            np.random.uniform(0.4, 0.9, n_per_type),    # rst_rate: 很高
            np.random.uniform(0.5, 0.9, n_per_type),    # syn_ratio: 高
            np.random.uniform(0.2, 0.5, n_per_type),    # new_conn_rate: 中高
            np.random.uniform(0.02, 0.06, n_per_type),  # avg_pkt_size: 小
            np.random.uniform(0.3, 0.8, n_per_type),    # rst_ratio: 高
        ])
        anomalies.append(port_scan)

        # 3. DDoS Flood: 极高 pps + bps
        ddos = np.column_stack([
            np.random.uniform(0.7, 1.0, n_per_type),    # pps: 极高
            np.random.uniform(0.6, 1.0, n_per_type),    # bps: 极高
            np.random.uniform(0.05, 0.2, n_per_type),   # syn_rate: 正常~略高
            np.random.uniform(0.0, 0.05, n_per_type),   # rst_rate: 低
            np.random.uniform(0.01, 0.05, n_per_type),  # syn_ratio: 正常
            np.random.uniform(0.05, 0.15, n_per_type),  # new_conn_rate: 正常
            np.random.uniform(0.4, 0.8, n_per_type),    # avg_pkt_size: 中~大
            np.random.uniform(0.0, 0.03, n_per_type),   # rst_ratio: 低
        ])
        anomalies.append(ddos)

        # 4. Slowloris: 高 new_conn_rate, 低流量
        slowloris = np.column_stack([
            np.random.uniform(0.01, 0.05, n_per_type),  # pps: 极低
            np.random.uniform(0.005, 0.02, n_per_type), # bps: 极低
            np.random.uniform(0.2, 0.5, n_per_type),    # syn_rate: 中高
            np.random.uniform(0.0, 0.02, n_per_type),   # rst_rate: 极低
            np.random.uniform(0.6, 0.95, n_per_type),   # syn_ratio: 高 (完成握手慢)
            np.random.uniform(0.3, 0.7, n_per_type),    # new_conn_rate: 高
            np.random.uniform(0.01, 0.05, n_per_type),  # avg_pkt_size: 极小
            np.random.uniform(0.0, 0.02, n_per_type),   # rst_ratio: 极低
        ])
        anomalies.append(slowloris)

        # 5. Amplification: 低 pps_in 但高 bps_out
        amplification = np.column_stack([
            np.random.uniform(0.05, 0.15, n_per_type),  # pps: 低
            np.random.uniform(0.5, 0.9, n_per_type),    # bps: 高 (大包)
            np.random.uniform(0.01, 0.05, n_per_type),  # syn_rate: 低
            np.random.uniform(0.0, 0.03, n_per_type),   # rst_rate: 低
            np.random.uniform(0.01, 0.05, n_per_type),  # syn_ratio: 正常
            np.random.uniform(0.01, 0.05, n_per_type),  # new_conn_rate: 低
            np.random.uniform(0.8, 1.0, n_per_type),    # avg_pkt_size: 极大
            np.random.uniform(0.0, 0.02, n_per_type),   # rst_ratio: 低
        ])
        anomalies.append(amplification)

        # 补足余数
        all_anomalies = np.vstack(anomalies)
        if len(all_anomalies) < n:
            extra = all_anomalies[np.random.choice(len(all_anomalies), n - len(all_anomalies))]
            all_anomalies = np.vstack([all_anomalies, extra])

        return all_anomalies[:n].clip(0, 1).astype(np.float32)

    def __len__(self):
        return len(self.data)

    def __getitem__(self, idx) -> Tuple[torch.Tensor, int]:
        return torch.from_numpy(self.data[idx]), int(self.labels[idx])


# ============================================================================
# DataLoader 工厂函数
# ============================================================================

def create_dataloaders(
    train_samples: int = 10000,
    val_samples: int = 2000,
    batch_size: int = 128,
    seed: int = 42,
) -> Tuple[DataLoader, DataLoader]:
    """创建合成数据的 DataLoader (无真实数据时使用)"""

    train_dataset = SyntheticSecurityDataset(
        n_samples=train_samples,
        anomaly_ratio=0.0,
        seed=seed,
    )

    val_dataset = SyntheticSecurityDataset(
        n_samples=val_samples,
        anomaly_ratio=0.3,
        seed=seed + 1,
    )

    train_loader = DataLoader(
        train_dataset,
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True,
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True,
    )

    return train_loader, val_loader


def create_dataloaders_from_npz(
    npz_path: str,
    train_ratio: float = 0.8,
    batch_size: int = 128,
    seed: int = 42,
) -> Dict[str, DataLoader]:
    """
    从 security_dataset.npz 创建 DataLoader

    数据分割策略 (利用标签信息):
      - 正常数据 (label=0): 80% 训练, 20% 验证
      - 异常数据 (label=1): 全部用于测试评估

    返回:
      {
        'train': DataLoader,   # 仅正常数据
        'val': DataLoader,     # 正常数据验证集
        'test': DataLoader,    # 正常 val + 全部异常 (用于阈值校准和评估)
      }
    """
    data = np.load(npz_path)
    inputs = data['inputs'].astype(np.float32)
    labels = data['labels'].astype(np.int64)

    np.random.seed(seed)

    # 分离正常和异常数据
    normal_mask = labels == 0
    anomaly_mask = labels == 1

    normal_inputs = inputs[normal_mask]
    anomaly_inputs = inputs[anomaly_mask]
    anomaly_labels = labels[anomaly_mask]

    # 正常数据随机分割
    n_normal = len(normal_inputs)
    perm = np.random.permutation(n_normal)
    split = int(n_normal * train_ratio)

    train_inputs = normal_inputs[perm[:split]]
    train_labels = np.zeros(split, dtype=np.int64)

    val_inputs = normal_inputs[perm[split:]]
    val_labels = np.zeros(n_normal - split, dtype=np.int64)

    # 测试集: 验证集中的正常数据 + 全部异常数据
    test_inputs = np.vstack([val_inputs, anomaly_inputs]) if len(anomaly_inputs) > 0 \
        else val_inputs
    test_labels = np.concatenate([val_labels, anomaly_labels]) if len(anomaly_inputs) > 0 \
        else val_labels

    print(f"  Data split:")
    print(f"    Train (normal only):  {len(train_inputs)}")
    print(f"    Val   (normal only):  {len(val_inputs)}")
    print(f"    Test  (normal + anomaly): {len(test_inputs)} "
          f"(normal={len(val_inputs)}, anomaly={len(anomaly_inputs)})")

    train_loader = DataLoader(
        SecurityDataset(train_inputs, train_labels),
        batch_size=batch_size,
        shuffle=True,
        num_workers=0,
        pin_memory=True,
    )
    val_loader = DataLoader(
        SecurityDataset(val_inputs, val_labels),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True,
    )
    test_loader = DataLoader(
        SecurityDataset(test_inputs, test_labels),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
        pin_memory=True,
    )

    return {
        'train': train_loader,
        'val': val_loader,
        'test': test_loader,
    }
