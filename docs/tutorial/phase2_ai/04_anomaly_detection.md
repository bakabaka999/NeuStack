# 教程 04：异常检测模型训练

> **前置要求**: 完成教程 01-03 (ONNX 集成已就绪)
> **目标**: 训练 LSTM-Autoencoder 异常检测模型，部署到 NeuStack

---

## 1. 异常检测原理

### 1.1 为什么用 Autoencoder

传统异常检测需要标注数据（正常/异常），但网络攻击种类繁多，难以穷举。

**Autoencoder 的思路**：
- 只用**正常流量**训练
- 模型学会"重构"正常模式
- 遇到异常流量时，重构误差显著升高

```
正常流量 ──→ Encoder ──→ Latent ──→ Decoder ──→ 重构 ≈ 原始  (误差小)
异常流量 ──→ Encoder ──→ Latent ──→ Decoder ──→ 重构 ≠ 原始  (误差大)
```

### 1.2 检测目标

| 攻击类型 | 特征表现 |
|----------|----------|
| SYN Flood | syn_rate 异常高，new_conn_rate 高但无后续数据 |
| 端口扫描 | rst_rate 高（大量连接被拒绝）|
| DDoS | packet_rate 异常高，avg_packet_size 可能异常 |
| 慢速攻击 | new_conn_rate 正常但连接持续时间异常 |

### 1.3 输入特征

我们使用 5 维特征向量（与 C++ 侧 `IAnomalyModel::Input` 对应）：

```python
features = [
    syn_rate,        # SYN 包速率 (归一化)
    rst_rate,        # RST 包速率 (归一化)
    new_conn_rate,   # 新建连接速率 (归一化)
    packet_rate,     # 总包速率 (归一化)
    avg_packet_size  # 平均包大小 (归一化)
]
```

---

## 2. 环境准备

### 2.1 目录结构

```
NeuStack/
├── training/                    # Python 训练代码
│   ├── anomaly/
│   │   ├── train.py            # 训练脚本
│   │   ├── model.py            # 模型定义
│   │   ├── dataset.py          # 数据加载
│   │   ├── export_onnx.py      # 导出 ONNX
│   │   └── config.yaml         # 超参数配置
│   ├── common/
│   │   └── utils.py            # 通用工具
│   └── requirements.txt
└── models/                      # 导出的 ONNX 模型
    └── anomaly_detector.onnx
```

### 2.2 Python 依赖

**方式 A：使用 venv**

```bash
# 创建虚拟环境
python3 -m venv venv
source venv/bin/activate

# 安装依赖
pip install torch numpy pandas matplotlib onnx onnxruntime pyyaml
```

**方式 B：使用 Conda（推荐）**

```bash
# 创建 conda 环境
conda create -n neustack python=3.11 -y
conda activate neustack

# 安装 PyTorch (根据平台选择)
# macOS / CPU
conda install pytorch -c pytorch -y

# Linux with CUDA
# conda install pytorch pytorch-cuda=12.1 -c pytorch -c nvidia -y

# 安装其他依赖
conda install numpy pandas matplotlib pyyaml -y
pip install onnx onnxruntime
```

**方式 C：使用 environment.yml**

```yaml
# training/environment.yml
name: neustack
channels:
  - pytorch
  - conda-forge
  - defaults
dependencies:
  - python=3.11
  - pytorch
  - numpy
  - pandas
  - matplotlib
  - pyyaml
  - pip
  - pip:
    - onnx
    - onnxruntime
```

```bash
# 从 yml 创建环境
conda env create -f environment.yml
conda activate neustack
```

**requirements.txt（兼容 pip）**：

```
# training/requirements.txt
torch>=2.0.0
numpy>=1.24.0
pandas>=2.0.0
matplotlib>=3.7.0
onnx>=1.14.0
onnxruntime>=1.15.0
pyyaml>=6.0
```

---

## 3. 数据准备

### 3.1 合成数据生成

为了快速验证，我们先用合成数据：

```python
# training/anomaly/dataset.py

import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader
from typing import Tuple, Optional

class SyntheticTrafficDataset(Dataset):
    """合成网络流量数据集"""

    def __init__(self, n_samples: int = 10000, anomaly_ratio: float = 0.0, seed: int = 42):
        """
        Args:
            n_samples: 样本数量
            anomaly_ratio: 异常样本比例 (训练时设为 0)
            seed: 随机种子
        """
        np.random.seed(seed)

        n_normal = int(n_samples * (1 - anomaly_ratio))
        n_anomaly = n_samples - n_normal

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
        n_samples=train_samples,
        anomaly_ratio=0.0,  # 训练只用正常数据
        seed=seed
    )

    # 验证集：混合正常和异常数据
    val_dataset = SyntheticTrafficDataset(
        n_samples=val_samples,
        anomaly_ratio=0.3,  # 30% 异常数据用于评估
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
```

### 3.2 从 NeuStack 采集真实数据

在生产环境中，我们需要从 NeuStack 采集真实流量数据。

**步骤 1**: 添加数据导出工具（C++ 侧）

```cpp
// include/neustack/metrics/metrics_exporter.hpp

#ifndef NEUSTACK_METRICS_EXPORTER_HPP
#define NEUSTACK_METRICS_EXPORTER_HPP

#include "neustack/metrics/global_metrics.hpp"
#include <fstream>
#include <chrono>

namespace neustack {

/**
 * 指标导出器 - 定期将 GlobalMetrics 快照导出到 CSV
 */
class MetricsExporter {
public:
    explicit MetricsExporter(const std::string& filepath)
        : _file(filepath), _prev_snapshot(global_metrics().snapshot())
    {
        // 写入 CSV 头
        _file << "timestamp_ms,syn_received,rst_received,conn_established,"
              << "conn_reset,packets_rx,bytes_rx\n";
    }

    ~MetricsExporter() {
        _file.close();
    }

    /**
     * 导出当前快照 (计算与上次的差值)
     * @param interval_ms 采样间隔 (毫秒)
     */
    void export_delta(uint64_t interval_ms) {
        auto snapshot = global_metrics().snapshot();
        auto delta = snapshot.diff(_prev_snapshot);

        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        _file << ms << ","
              << delta.syn_received << ","
              << delta.rst_received << ","
              << delta.conn_established << ","
              << delta.conn_reset << ","
              << delta.packets_rx << ","
              << delta.bytes_rx << "\n";

        _prev_snapshot = snapshot;
    }

    void flush() { _file.flush(); }

private:
    std::ofstream _file;
    GlobalMetrics::Snapshot _prev_snapshot;
};

} // namespace neustack

#endif
```

**步骤 2**: 在 main.cpp 中集成

```cpp
// 在 main.cpp 顶部添加
#include "neustack/metrics/metric_exporter.hpp"

// 在 Config 结构体中添加
struct Config {
    // ... 已有字段 ...
    std::string export_path;  // 导出路径，空 = 不导出
};

// 在 parse_args 中添加选项
if (std::strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
    cfg.export_path = argv[++i];
}

// 修改 run_event_loop 函数签名，传入 exporter
static void run_event_loop(NetDevice &device, IPv4Layer &ip_layer,
                           TCPLayer &tcp_layer, DNSClient &dns,
                           HttpClient &http,
                           MetricsExporter *exporter)  // 新增参数
{
    // ... 已有代码 ...

    // 在定时器部分添加导出
    auto now = std::chrono::steady_clock::now();
    if (now - last_timer >= TIMER_INTERVAL) {
        tcp_layer.on_timer();
        dns.on_timer();

        // 导出指标
        if (exporter) {
            exporter->export_delta(100);
        }

        last_timer = now;
    }

    // ... 已有代码 ...
}

// 在 main() 中创建 exporter
int main(int argc, char *argv[]) {
    // ... 已有代码 ...

    // 创建导出器 (可选)
    std::unique_ptr<MetricsExporter> exporter;
    if (!cfg.export_path.empty()) {
        exporter = std::make_unique<MetricsExporter>(cfg.export_path);
        LOG_INFO(APP, "exporting metrics to: %s", cfg.export_path.c_str());
    }

    // 主循环
    run_event_loop(*device, ip_layer, tcp, dns, http_client, exporter.get());

    // ... 已有代码 ...
}
```

**步骤 3**: 运行并采集数据

```bash
# 启动 NeuStack 并导出数据
./neustack --export traffic_data.csv

# 在另一个终端产生流量
curl http://192.168.100.2/
ping 192.168.100.2

# Ctrl+C 停止后，数据保存在 traffic_data.csv
cat traffic_data.csv
# timestamp_ms,syn_received,rst_received,conn_established,conn_reset,packets_rx,bytes_rx
# 1234567890,2,0,1,0,15,1024
# 1234567990,0,0,0,0,3,256
# ...
```

**步骤 4**: Python 加载真实数据

```python
# training/anomaly/dataset.py (补充)

import pandas as pd

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
```

---

## 4. 模型架构

### 4.1 LSTM-Autoencoder

```python
# training/anomaly/model.py

import torch
import torch.nn as nn

class LSTMAutoencoder(nn.Module):
    """
    LSTM-Autoencoder for anomaly detection

    虽然我们的输入是单个时间点的特征向量（非序列），
    但使用 LSTM 可以在未来扩展到时序异常检测。
    当前实现将单个特征向量视为长度为 1 的序列。
    """

    def __init__(
        self,
        input_dim: int = 5,
        hidden_dim: int = 32,
        latent_dim: int = 8,
        num_layers: int = 1,
        dropout: float = 0.1
    ):
        super().__init__()

        self.input_dim = input_dim
        self.hidden_dim = hidden_dim
        self.latent_dim = latent_dim
        self.num_layers = num_layers

        # Encoder
        self.encoder_fc = nn.Linear(input_dim, hidden_dim)
        self.encoder_lstm = nn.LSTM(
            hidden_dim, hidden_dim,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0
        )
        self.encoder_latent = nn.Linear(hidden_dim, latent_dim)

        # Decoder
        self.decoder_latent = nn.Linear(latent_dim, hidden_dim)
        self.decoder_lstm = nn.LSTM(
            hidden_dim, hidden_dim,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0
        )
        self.decoder_fc = nn.Linear(hidden_dim, input_dim)

        # Activation
        self.relu = nn.ReLU()
        self.sigmoid = nn.Sigmoid()

    def encode(self, x: torch.Tensor) -> torch.Tensor:
        """编码器: x -> latent"""
        # x: [batch, input_dim]
        x = self.relu(self.encoder_fc(x))           # [batch, hidden_dim]
        x = x.unsqueeze(1)                           # [batch, 1, hidden_dim]
        x, _ = self.encoder_lstm(x)                  # [batch, 1, hidden_dim]
        x = x.squeeze(1)                             # [batch, hidden_dim]
        latent = self.encoder_latent(x)              # [batch, latent_dim]
        return latent

    def decode(self, latent: torch.Tensor) -> torch.Tensor:
        """解码器: latent -> reconstructed"""
        x = self.relu(self.decoder_latent(latent))  # [batch, hidden_dim]
        x = x.unsqueeze(1)                           # [batch, 1, hidden_dim]
        x, _ = self.decoder_lstm(x)                  # [batch, 1, hidden_dim]
        x = x.squeeze(1)                             # [batch, hidden_dim]
        reconstructed = self.sigmoid(self.decoder_fc(x))  # [batch, input_dim], 范围 [0,1]
        return reconstructed

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """前向传播: x -> reconstructed"""
        latent = self.encode(x)
        reconstructed = self.decode(latent)
        return reconstructed

    def reconstruction_error(self, x: torch.Tensor) -> torch.Tensor:
        """计算重构误差 (MSE)"""
        reconstructed = self.forward(x)
        error = torch.mean((x - reconstructed) ** 2, dim=1)
        return error


class SimpleAutoencoder(nn.Module):
    """
    简化版 Autoencoder (纯 MLP)

    更轻量，适合单点特征检测
    """

    def __init__(
        self,
        input_dim: int = 5,
        hidden_dims: list = [32, 16],
        latent_dim: int = 8
    ):
        super().__init__()

        self.input_dim = input_dim

        # Encoder
        encoder_layers = []
        prev_dim = input_dim
        for dim in hidden_dims:
            encoder_layers.extend([
                nn.Linear(prev_dim, dim),
                nn.ReLU(),
            ])
            prev_dim = dim
        encoder_layers.append(nn.Linear(prev_dim, latent_dim))
        self.encoder = nn.Sequential(*encoder_layers)

        # Decoder (镜像结构)
        decoder_layers = []
        prev_dim = latent_dim
        for dim in reversed(hidden_dims):
            decoder_layers.extend([
                nn.Linear(prev_dim, dim),
                nn.ReLU(),
            ])
            prev_dim = dim
        decoder_layers.extend([
            nn.Linear(prev_dim, input_dim),
            nn.Sigmoid()  # 输出范围 [0, 1]
        ])
        self.decoder = nn.Sequential(*decoder_layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        latent = self.encoder(x)
        reconstructed = self.decoder(latent)
        return reconstructed

    def reconstruction_error(self, x: torch.Tensor) -> torch.Tensor:
        reconstructed = self.forward(x)
        error = torch.mean((x - reconstructed) ** 2, dim=1)
        return error
```

### 4.2 模型选择

| 模型 | 参数量 | 推理延迟 | 适用场景 |
|------|--------|----------|----------|
| SimpleAutoencoder | ~1.5K | < 0.1ms | 单点检测，资源受限 |
| LSTMAutoencoder | ~5K | < 0.5ms | 可扩展到时序检测 |

**推荐**：从 `SimpleAutoencoder` 开始，验证流程后再尝试 LSTM 版本。

---

## 5. 训练脚本

### 5.1 配置文件

```yaml
# training/anomaly/config.yaml

model:
  type: "simple"  # "simple" 或 "lstm"
  input_dim: 5
  hidden_dims: [32, 16]
  latent_dim: 8

training:
  epochs: 100
  batch_size: 64
  learning_rate: 0.001
  weight_decay: 0.0001
  early_stopping_patience: 10

data:
  train_samples: 8000
  val_samples: 2000
  seed: 42

output:
  model_dir: "checkpoints"
  onnx_path: "../../models/anomaly_detector.onnx"
```

### 5.2 训练脚本

```python
# training/anomaly/train.py

import os
import yaml
import argparse
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
import matplotlib.pyplot as plt
from typing import Dict, Tuple

from model import SimpleAutoencoder, LSTMAutoencoder
from dataset import create_dataloaders


def load_config(config_path: str) -> Dict:
    """加载配置文件"""
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config: Dict) -> nn.Module:
    """根据配置创建模型"""
    model_type = config['model']['type']

    if model_type == 'simple':
        return SimpleAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model']['hidden_dims'],
            latent_dim=config['model']['latent_dim']
        )
    elif model_type == 'lstm':
        return LSTMAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model'].get('hidden_dim', 32),
            latent_dim=config['model']['latent_dim'],
            num_layers=config['model'].get('num_layers', 1)
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def train_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: optim.Optimizer,
    device: torch.device
) -> float:
    """训练一个 epoch"""
    model.train()
    total_loss = 0.0

    for batch, _ in loader:
        batch = batch.to(device)

        # 前向传播
        reconstructed = model(batch)
        loss = nn.functional.mse_loss(reconstructed, batch)

        # 反向传播
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item() * batch.size(0)

    return total_loss / len(loader.dataset)


def validate(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device
) -> Tuple[float, np.ndarray, np.ndarray]:
    """验证并返回重构误差"""
    model.eval()
    total_loss = 0.0
    all_errors = []
    all_labels = []

    with torch.no_grad():
        for batch, labels in loader:
            batch = batch.to(device)

            # 计算重构误差
            reconstructed = model(batch)
            errors = torch.mean((batch - reconstructed) ** 2, dim=1)

            loss = errors.mean()
            total_loss += loss.item() * batch.size(0)

            all_errors.extend(errors.cpu().numpy())
            all_labels.extend(labels.numpy())

    return (
        total_loss / len(loader.dataset),
        np.array(all_errors),
        np.array(all_labels)
    )


def compute_threshold(errors: np.ndarray, labels: np.ndarray, percentile: float = 95) -> float:
    """
    计算异常检测阈值

    方法：使用正常样本重构误差的 percentile 分位数
    """
    normal_errors = errors[labels == 0]
    threshold = np.percentile(normal_errors, percentile)
    return threshold


def evaluate_detection(
    errors: np.ndarray,
    labels: np.ndarray,
    threshold: float
) -> Dict[str, float]:
    """评估异常检测性能"""
    predictions = (errors > threshold).astype(int)

    tp = np.sum((predictions == 1) & (labels == 1))
    tn = np.sum((predictions == 0) & (labels == 0))
    fp = np.sum((predictions == 1) & (labels == 0))
    fn = np.sum((predictions == 0) & (labels == 1))

    accuracy = (tp + tn) / len(labels) if len(labels) > 0 else 0
    precision = tp / (tp + fp) if (tp + fp) > 0 else 0
    recall = tp / (tp + fn) if (tp + fn) > 0 else 0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

    return {
        'accuracy': accuracy,
        'precision': precision,
        'recall': recall,
        'f1': f1,
        'threshold': threshold
    }


def plot_training_history(train_losses: list, val_losses: list, save_path: str):
    """绘制训练曲线"""
    plt.figure(figsize=(10, 5))
    plt.plot(train_losses, label='Train Loss')
    plt.plot(val_losses, label='Val Loss')
    plt.xlabel('Epoch')
    plt.ylabel('MSE Loss')
    plt.title('Training History')
    plt.legend()
    plt.grid(True)
    plt.savefig(save_path)
    plt.close()


def plot_error_distribution(
    errors: np.ndarray,
    labels: np.ndarray,
    threshold: float,
    save_path: str
):
    """绘制重构误差分布"""
    plt.figure(figsize=(10, 5))

    normal_errors = errors[labels == 0]
    anomaly_errors = errors[labels == 1]

    plt.hist(normal_errors, bins=50, alpha=0.7, label='Normal', color='blue')
    if len(anomaly_errors) > 0:
        plt.hist(anomaly_errors, bins=50, alpha=0.7, label='Anomaly', color='red')

    plt.axvline(x=threshold, color='green', linestyle='--', label=f'Threshold={threshold:.4f}')

    plt.xlabel('Reconstruction Error (MSE)')
    plt.ylabel('Count')
    plt.title('Reconstruction Error Distribution')
    plt.legend()
    plt.grid(True)
    plt.savefig(save_path)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='Train anomaly detection model')
    parser.add_argument('--config', type=str, default='config.yaml', help='Config file path')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 设备
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # 创建输出目录
    os.makedirs(config['output']['model_dir'], exist_ok=True)

    # 创建数据加载器
    train_loader, val_loader = create_dataloaders(
        train_samples=config['data']['train_samples'],
        val_samples=config['data']['val_samples'],
        batch_size=config['training']['batch_size'],
        seed=config['data']['seed']
    )
    print(f"Train samples: {len(train_loader.dataset)}, Val samples: {len(val_loader.dataset)}")

    # 创建模型
    model = create_model(config).to(device)
    print(f"Model: {type(model).__name__}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    # 优化器
    optimizer = optim.Adam(
        model.parameters(),
        lr=config['training']['learning_rate'],
        weight_decay=config['training']['weight_decay']
    )

    # 训练循环
    train_losses = []
    val_losses = []
    best_val_loss = float('inf')
    patience_counter = 0

    for epoch in range(config['training']['epochs']):
        # 训练
        train_loss = train_epoch(model, train_loader, optimizer, device)
        train_losses.append(train_loss)

        # 验证
        val_loss, val_errors, val_labels = validate(model, val_loader, device)
        val_losses.append(val_loss)

        # 打印进度
        print(f"Epoch {epoch+1:3d}/{config['training']['epochs']}: "
              f"Train Loss = {train_loss:.6f}, Val Loss = {val_loss:.6f}")

        # Early stopping
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            patience_counter = 0

            # 保存最佳模型
            torch.save(model.state_dict(),
                      os.path.join(config['output']['model_dir'], 'best_model.pth'))
        else:
            patience_counter += 1
            if patience_counter >= config['training']['early_stopping_patience']:
                print(f"Early stopping at epoch {epoch+1}")
                break

    # 加载最佳模型
    model.load_state_dict(
        torch.load(os.path.join(config['output']['model_dir'], 'best_model.pth'))
    )

    # 最终评估
    print("\n" + "=" * 50)
    print("Final Evaluation")
    print("=" * 50)

    _, val_errors, val_labels = validate(model, val_loader, device)

    # 计算阈值
    threshold = compute_threshold(val_errors, val_labels, percentile=95)
    print(f"Threshold (95th percentile of normal errors): {threshold:.6f}")

    # 评估检测性能
    metrics = evaluate_detection(val_errors, val_labels, threshold)
    print(f"Accuracy:  {metrics['accuracy']:.4f}")
    print(f"Precision: {metrics['precision']:.4f}")
    print(f"Recall:    {metrics['recall']:.4f}")
    print(f"F1 Score:  {metrics['f1']:.4f}")

    # 绘制图表
    plot_training_history(
        train_losses, val_losses,
        os.path.join(config['output']['model_dir'], 'training_history.png')
    )

    plot_error_distribution(
        val_errors, val_labels, threshold,
        os.path.join(config['output']['model_dir'], 'error_distribution.png')
    )

    print(f"\nPlots saved to {config['output']['model_dir']}/")

    # 保存阈值到配置
    with open(os.path.join(config['output']['model_dir'], 'threshold.txt'), 'w') as f:
        f.write(f"{threshold:.6f}\n")

    print(f"Threshold saved to {config['output']['model_dir']}/threshold.txt")
    print("\nTraining complete!")


if __name__ == '__main__':
    main()
```

### 5.3 运行训练

```bash
cd training/anomaly
python train.py --config config.yaml
```

预期输出：
```
Using device: cpu
Train samples: 8000, Val samples: 2000
Model: SimpleAutoencoder
Parameters: 1,477

Epoch   1/100: Train Loss = 0.042351, Val Loss = 0.038924
Epoch   2/100: Train Loss = 0.031245, Val Loss = 0.029873
...
Epoch  45/100: Train Loss = 0.001234, Val Loss = 0.001456
Early stopping at epoch 45

==================================================
Final Evaluation
==================================================
Threshold (95th percentile of normal errors): 0.003421
Accuracy:  0.9450
Precision: 0.8923
Recall:    0.9167
F1 Score:  0.9043

Plots saved to checkpoints/
Threshold saved to checkpoints/threshold.txt

Training complete!
```

---

## 6. 导出 ONNX

### 6.1 导出脚本

```python
# training/anomaly/export_onnx.py

import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from model import SimpleAutoencoder, LSTMAutoencoder


def load_config(config_path: str):
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config):
    model_type = config['model']['type']

    if model_type == 'simple':
        return SimpleAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model']['hidden_dims'],
            latent_dim=config['model']['latent_dim']
        )
    elif model_type == 'lstm':
        return LSTMAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model'].get('hidden_dim', 32),
            latent_dim=config['model']['latent_dim'],
            num_layers=config['model'].get('num_layers', 1)
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def export_onnx(model, input_dim, output_path):
    """导出模型到 ONNX 格式"""
    model.eval()

    # 创建示例输入
    dummy_input = torch.randn(1, input_dim)

    # 导出
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={
            'input': {0: 'batch_size'},
            'output': {0: 'batch_size'}
        },
        opset_version=17,
        do_constant_folding=True
    )

    print(f"Exported to {output_path}")

    # 验证导出的模型
    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")

    return output_path


def verify_onnx(pytorch_model, onnx_path, input_dim):
    """验证 ONNX 模型与 PyTorch 模型输出一致"""
    pytorch_model.eval()

    # 创建测试输入
    test_input = np.random.randn(5, input_dim).astype(np.float32)

    # PyTorch 推理
    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    # ONNX Runtime 推理
    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'input': test_input})[0]

    # 比较输出
    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max difference between PyTorch and ONNX: {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED!")
        return True
    else:
        print("WARNING: Outputs differ significantly!")
        return False


def main():
    parser = argparse.ArgumentParser(description='Export anomaly model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/best_model.pth')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 创建并加载模型
    model = create_model(config)
    model.load_state_dict(torch.load(args.checkpoint, map_location='cpu'))
    model.eval()
    print(f"Loaded model from {args.checkpoint}")

    # 导出 ONNX
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    export_onnx(model, config['model']['input_dim'], output_path)

    # 验证
    verify_onnx(model, output_path, config['model']['input_dim'])

    # 打印模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}, shape: {[d.dim_value for d in onnx_model.graph.input[0].type.tensor_type.shape.dim]}")
    print(f"Output: {onnx_model.graph.output[0].name}, shape: {[d.dim_value for d in onnx_model.graph.output[0].type.tensor_type.shape.dim]}")
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")


if __name__ == '__main__':
    main()
```

### 6.2 运行导出

```bash
python export_onnx.py --config config.yaml --checkpoint checkpoints/best_model.pth
```

预期输出：
```
Loaded model from checkpoints/best_model.pth
Exported to ../../models/anomaly_detector.onnx
ONNX model validation passed!
Max difference between PyTorch and ONNX: 0.00000012
Verification PASSED!

==================================================
ONNX Model Info
==================================================
Input: input, shape: [1, 5]
Output: output, shape: [1, 5]
File size: 8.34 KB
```

---

## 7. 阈值校准

### 7.1 阈值选择策略

| 策略 | 阈值设定 | 特点 |
|------|----------|------|
| 固定百分位 | 正常样本的 95th/99th percentile | 简单，可能误报 |
| 均值 + k*标准差 | μ + 3σ | 假设高斯分布 |
| 最大化 F1 | 搜索最优阈值 | 需要标注数据 |
| 业务驱动 | 根据误报容忍度调整 | 实际部署 |

### 7.2 阈值调优脚本

```python
# training/anomaly/calibrate_threshold.py

import numpy as np
import matplotlib.pyplot as plt
from sklearn.metrics import precision_recall_curve, roc_curve, auc


def find_optimal_threshold(errors: np.ndarray, labels: np.ndarray) -> float:
    """找到最大化 F1 的阈值"""
    precision, recall, thresholds = precision_recall_curve(labels, errors)

    # 计算 F1
    f1_scores = 2 * precision * recall / (precision + recall + 1e-8)

    # 找最大 F1 对应的阈值
    best_idx = np.argmax(f1_scores[:-1])  # 最后一个值是 recall=0 的点
    best_threshold = thresholds[best_idx]
    best_f1 = f1_scores[best_idx]

    print(f"Optimal threshold: {best_threshold:.6f}")
    print(f"Best F1 score: {best_f1:.4f}")

    return best_threshold


def plot_roc_pr_curves(errors: np.ndarray, labels: np.ndarray, save_dir: str):
    """绘制 ROC 和 PR 曲线"""
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    # ROC 曲线
    fpr, tpr, _ = roc_curve(labels, errors)
    roc_auc = auc(fpr, tpr)

    axes[0].plot(fpr, tpr, label=f'ROC (AUC = {roc_auc:.3f})')
    axes[0].plot([0, 1], [0, 1], 'k--')
    axes[0].set_xlabel('False Positive Rate')
    axes[0].set_ylabel('True Positive Rate')
    axes[0].set_title('ROC Curve')
    axes[0].legend()
    axes[0].grid(True)

    # PR 曲线
    precision, recall, _ = precision_recall_curve(labels, errors)
    pr_auc = auc(recall, precision)

    axes[1].plot(recall, precision, label=f'PR (AUC = {pr_auc:.3f})')
    axes[1].set_xlabel('Recall')
    axes[1].set_ylabel('Precision')
    axes[1].set_title('Precision-Recall Curve')
    axes[1].legend()
    axes[1].grid(True)

    plt.tight_layout()
    plt.savefig(f'{save_dir}/roc_pr_curves.png')
    plt.close()

    print(f"ROC AUC: {roc_auc:.4f}")
    print(f"PR AUC: {pr_auc:.4f}")
```

---

## 8. 集成测试

训练完成后，使用 `ai_test` 验证：

```bash
# 确保模型已导出到 models/anomaly_detector.onnx
cd build
./examples/ai_test
```

预期看到异常检测动作：
```
=== AI Actions ===
  [CWND] alpha=-0.187024
  [ANOMALY] score=0.0523    # 如果触发了异常检测
  [BW_PRED] predicted=47882284 bytes/s
```

### 8.1 更新 C++ 侧阈值

训练后得到的最优阈值需要更新到 C++ 配置中：

```cpp
IntelligencePlaneConfig config;
config.anomaly_model_path = "models/anomaly_detector.onnx";
config.anomaly_threshold = 0.003421f;  // 从 threshold.txt 读取
```

---

## 9. 完整训练流程

```bash
# 1. 进入训练目录
cd training/anomaly

# 2. 训练模型
python train.py --config config.yaml

# 3. 导出 ONNX
python export_onnx.py --config config.yaml

# 4. 回到 build 目录测试
cd ../../build
./examples/ai_test

# 5. 查看训练结果
ls ../training/anomaly/checkpoints/
# best_model.pth  training_history.png  error_distribution.png  threshold.txt
```

---

## 10. 新增文件清单

```
training/
├── anomaly/
│   ├── config.yaml          # 超参数配置
│   ├── dataset.py           # 数据集定义
│   ├── model.py             # 模型架构
│   ├── train.py             # 训练脚本
│   ├── export_onnx.py       # ONNX 导出
│   └── calibrate_threshold.py  # 阈值校准
├── common/
│   └── utils.py             # 通用工具 (可选)
└── requirements.txt         # Python 依赖

models/
└── anomaly_detector.onnx    # 训练后的模型 (覆盖测试模型)
```

---

## 11. 下一步

- **教程 05: 带宽预测模型训练** — LSTM 时序预测
- **教程 06: Orca 拥塞控制训练** — DDPG 强化学习

---

## 12. 参考资料

- [Autoencoder for Anomaly Detection](https://arxiv.org/abs/2003.05991)
- [PyTorch ONNX Export](https://pytorch.org/docs/stable/onnx.html)
- [Network Intrusion Detection with Autoencoders](https://www.sciencedirect.com/science/article/pii/S0167404821000444)
