# 教程 05：带宽预测模型训练

> **前置要求**: 完成教程 01-04
> **目标**: 训练 LSTM 带宽预测模型，预测未来可用带宽

---

## 1. 带宽预测原理

### 1.1 为什么需要带宽预测

传统拥塞控制（如 CUBIC）是**被动响应**：丢包后才降速。

带宽预测实现**主动调整**：
- 预测网络状况变化
- 提前调整发送速率
- 减少丢包和延迟抖动

```
传统方式:  发送 → 丢包 → 降速 → 恢复 → 发送 → 丢包 ...  (锯齿形)
预测方式:  发送 → 预测带宽下降 → 提前降速 → 平滑过渡      (更稳定)
```

### 1.2 预测目标

| 输入 | 输出 |
|------|------|
| 过去 N 个时间步的 (throughput, RTT, loss_rate) | 下一时间步的预测带宽 |

### 1.3 模型选择

| 模型 | 优点 | 缺点 | 适用 |
|------|------|------|------|
| **LSTM** | 捕获长期依赖，处理时序 | 训练慢，参数多 | 默认选择 |
| GRU | 比 LSTM 轻量 | 性能略低 | 资源受限 |
| Transformer | 并行训练，长序列 | 推理重 | 超长序列 |
| 1D CNN | 快速，局部特征 | 长期依赖弱 | 短序列 |

---

## 2. 环境准备

### 2.1 目录结构

```
NeuStack/
├── training/
│   ├── bandwidth/
│   │   ├── train.py            # 训练脚本
│   │   ├── model.py            # LSTM 模型
│   │   ├── dataset.py          # 数据集
│   │   ├── export_onnx.py      # 导出 ONNX
│   │   └── config.yaml         # 配置
│   └── anomaly/                # 教程 04
└── models/
    └── bandwidth_predictor.onnx
```

### 2.2 依赖

与教程 04 相同，无需额外安装。

---

## 3. 数据准备

### 3.1 输入特征

每个时间步 3 个特征（与 C++ 侧 `IBandwidthModel::Input` 对应）：

```python
# 时间步 t 的特征
features_t = [
    throughput_normalized,  # 吞吐量 / MAX_THROUGHPUT
    rtt_normalized,         # RTT / MAX_RTT
    loss_rate               # 丢包率 [0, 1]
]

# 输入序列: [history_length, 3]
# 例如 history_length=10，则输入形状 [10, 3]
```

### 3.2 合成数据生成

```python
# training/bandwidth/dataset.py

import numpy as np
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
```

### 3.3 从 NeuStack 采集真实数据

使用教程 04 的 `MetricsExporter` 导出 `TCPSample`，然后在 Python 中处理：

```python
# training/bandwidth/dataset.py (补充)

import pandas as pd

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
```

---

## 4. 模型架构

### 4.1 LSTM 预测器

```python
# training/bandwidth/model.py

import torch
import torch.nn as nn

class LSTMBandwidthPredictor(nn.Module):
    """
    LSTM 带宽预测模型

    输入: [batch, seq_len, input_dim]  (时序特征)
    输出: [batch, 1]                    (预测带宽)
    """

    def __init__(
        self,
        input_dim: int = 3,           # throughput, rtt, loss
        hidden_dim: int = 64,
        num_layers: int = 2,
        dropout: float = 0.2,
        bidirectional: bool = False
    ):
        super().__init__()

        self.input_dim = input_dim
        self.hidden_dim = hidden_dim
        self.num_layers = num_layers
        self.bidirectional = bidirectional

        # LSTM 层
        self.lstm = nn.LSTM(
            input_size=input_dim,
            hidden_size=hidden_dim,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0,
            bidirectional=bidirectional
        )

        # 输出层
        lstm_output_dim = hidden_dim * (2 if bidirectional else 1)
        self.fc = nn.Sequential(
            nn.Linear(lstm_output_dim, hidden_dim // 2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_dim // 2, 1),
            nn.Sigmoid()  # 输出范围 [0, 1]
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """
        Args:
            x: [batch, seq_len, input_dim]
        Returns:
            [batch, 1] 预测的归一化带宽
        """
        # LSTM 编码
        lstm_out, (h_n, c_n) = self.lstm(x)

        # 取最后一个时间步的输出
        if self.bidirectional:
            # 拼接前向和后向的最后隐藏状态
            last_hidden = torch.cat([h_n[-2], h_n[-1]], dim=1)
        else:
            last_hidden = h_n[-1]

        # 预测
        output = self.fc(last_hidden)
        return output


class SimpleLSTMPredictor(nn.Module):
    """
    简化版 LSTM 预测器

    更轻量，适合嵌入式部署
    """

    def __init__(
        self,
        input_dim: int = 3,
        hidden_dim: int = 32,
        num_layers: int = 1
    ):
        super().__init__()

        self.lstm = nn.LSTM(
            input_size=input_dim,
            hidden_size=hidden_dim,
            num_layers=num_layers,
            batch_first=True
        )

        self.fc = nn.Sequential(
            nn.Linear(hidden_dim, 1),
            nn.Sigmoid()
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        lstm_out, (h_n, _) = self.lstm(x)
        output = self.fc(h_n[-1])
        return output
```

### 4.2 模型对比

| 模型 | 参数量 | 推理延迟 | MAE |
|------|--------|----------|-----|
| SimpleLSTMPredictor | ~2K | < 0.5ms | ~0.05 |
| LSTMBandwidthPredictor | ~20K | < 1ms | ~0.03 |

---

## 5. 训练脚本

### 5.1 配置文件

```yaml
# training/bandwidth/config.yaml

model:
  type: "simple"  # "simple" 或 "full"
  input_dim: 3
  hidden_dim: 32
  num_layers: 1

training:
  epochs: 100
  batch_size: 64
  learning_rate: 0.001
  weight_decay: 0.0001
  early_stopping_patience: 15

data:
  history_length: 10
  train_samples: 8000
  val_samples: 2000
  seed: 42

output:
  model_dir: "checkpoints"
  onnx_path: "../../models/bandwidth_predictor.onnx"
```

### 5.2 训练脚本

```python
# training/bandwidth/train.py

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

from model import SimpleLSTMPredictor, LSTMBandwidthPredictor
from dataset import create_dataloaders


def load_config(config_path: str) -> Dict:
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config: Dict) -> nn.Module:
    model_type = config['model']['type']

    if model_type == 'simple':
        return SimpleLSTMPredictor(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model']['hidden_dim'],
            num_layers=config['model']['num_layers']
        )
    elif model_type == 'full':
        return LSTMBandwidthPredictor(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model']['hidden_dim'],
            num_layers=config['model']['num_layers'],
            dropout=config['model'].get('dropout', 0.2)
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def train_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: optim.Optimizer,
    criterion: nn.Module,
    device: torch.device
) -> float:
    model.train()
    total_loss = 0.0

    for sequences, targets in loader:
        sequences = sequences.to(device)
        targets = targets.to(device).unsqueeze(1)

        # 前向传播
        predictions = model(sequences)
        loss = criterion(predictions, targets)

        # 反向传播
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item() * sequences.size(0)

    return total_loss / len(loader.dataset)


def validate(
    model: nn.Module,
    loader: DataLoader,
    criterion: nn.Module,
    device: torch.device
) -> Tuple[float, float]:
    model.eval()
    total_loss = 0.0
    total_mae = 0.0

    with torch.no_grad():
        for sequences, targets in loader:
            sequences = sequences.to(device)
            targets = targets.to(device).unsqueeze(1)

            predictions = model(sequences)
            loss = criterion(predictions, targets)

            total_loss += loss.item() * sequences.size(0)
            total_mae += torch.abs(predictions - targets).sum().item()

    n_samples = len(loader.dataset)
    return total_loss / n_samples, total_mae / n_samples


def plot_training_history(train_losses: list, val_losses: list, save_path: str):
    plt.figure(figsize=(10, 5))
    plt.plot(train_losses, label='Train Loss')
    plt.plot(val_losses, label='Val Loss')
    plt.xlabel('Epoch')
    plt.ylabel('MSE Loss')
    plt.title('Bandwidth Prediction Training History')
    plt.legend()
    plt.grid(True)
    plt.savefig(save_path)
    plt.close()


def plot_predictions(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    save_path: str,
    n_samples: int = 100
):
    """绘制预测 vs 真实值"""
    model.eval()

    all_preds = []
    all_targets = []

    with torch.no_grad():
        for sequences, targets in loader:
            sequences = sequences.to(device)
            predictions = model(sequences)

            all_preds.extend(predictions.cpu().numpy().flatten())
            all_targets.extend(targets.numpy())

            if len(all_preds) >= n_samples:
                break

    all_preds = np.array(all_preds[:n_samples])
    all_targets = np.array(all_targets[:n_samples])

    plt.figure(figsize=(12, 5))

    # 时序图
    plt.subplot(1, 2, 1)
    plt.plot(all_targets, label='Actual', alpha=0.7)
    plt.plot(all_preds, label='Predicted', alpha=0.7)
    plt.xlabel('Sample')
    plt.ylabel('Normalized Bandwidth')
    plt.title('Prediction vs Actual')
    plt.legend()
    plt.grid(True)

    # 散点图
    plt.subplot(1, 2, 2)
    plt.scatter(all_targets, all_preds, alpha=0.5, s=10)
    plt.plot([0, 1], [0, 1], 'r--', label='Perfect')
    plt.xlabel('Actual')
    plt.ylabel('Predicted')
    plt.title('Prediction Scatter Plot')
    plt.legend()
    plt.grid(True)

    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='Train bandwidth prediction model')
    parser.add_argument('--config', type=str, default='config.yaml')
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
        history_length=config['data']['history_length'],
        batch_size=config['training']['batch_size'],
        seed=config['data']['seed']
    )
    print(f"Train samples: {len(train_loader.dataset)}, Val samples: {len(val_loader.dataset)}")
    print(f"History length: {config['data']['history_length']}")

    # 创建模型
    model = create_model(config).to(device)
    print(f"Model: {type(model).__name__}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    # 损失函数和优化器
    criterion = nn.MSELoss()
    optimizer = optim.Adam(
        model.parameters(),
        lr=config['training']['learning_rate'],
        weight_decay=config['training']['weight_decay']
    )

    # 学习率调度
    scheduler = optim.lr_scheduler.ReduceLROnPlateau(
        optimizer, mode='min', factor=0.5, patience=5, verbose=True
    )

    # 训练循环
    train_losses = []
    val_losses = []
    best_val_loss = float('inf')
    patience_counter = 0

    for epoch in range(config['training']['epochs']):
        # 训练
        train_loss = train_epoch(model, train_loader, optimizer, criterion, device)
        train_losses.append(train_loss)

        # 验证
        val_loss, val_mae = validate(model, val_loader, criterion, device)
        val_losses.append(val_loss)

        # 学习率调度
        scheduler.step(val_loss)

        # 打印进度
        print(f"Epoch {epoch+1:3d}/{config['training']['epochs']}: "
              f"Train Loss = {train_loss:.6f}, Val Loss = {val_loss:.6f}, Val MAE = {val_mae:.4f}")

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
        torch.load(os.path.join(config['output']['model_dir'], 'best_model.pth'),
                   map_location=device, weights_only=True)
    )

    # 最终评估
    print("\n" + "=" * 50)
    print("Final Evaluation")
    print("=" * 50)

    val_loss, val_mae = validate(model, val_loader, criterion, device)
    print(f"Val Loss (MSE): {val_loss:.6f}")
    print(f"Val MAE: {val_mae:.4f}")

    # RMSE
    rmse = np.sqrt(val_loss)
    print(f"Val RMSE: {rmse:.4f}")

    # 绘制图表
    plot_training_history(
        train_losses, val_losses,
        os.path.join(config['output']['model_dir'], 'training_history.png')
    )

    plot_predictions(
        model, val_loader, device,
        os.path.join(config['output']['model_dir'], 'predictions.png')
    )

    print(f"\nPlots saved to {config['output']['model_dir']}/")
    print("\nTraining complete!")


if __name__ == '__main__':
    main()
```

### 5.3 运行训练

```bash
cd training/bandwidth
python train.py --config config.yaml
```

预期输出：
```
Using device: cpu
Train samples: 8000, Val samples: 2000
History length: 10
Model: SimpleLSTMPredictor
Parameters: 4,609

Epoch   1/100: Train Loss = 0.045231, Val Loss = 0.038924, Val MAE = 0.1523
Epoch   2/100: Train Loss = 0.021245, Val Loss = 0.019873, Val MAE = 0.1021
...
Epoch  45/100: Train Loss = 0.002134, Val Loss = 0.002456, Val MAE = 0.0389
Early stopping at epoch 45

==================================================
Final Evaluation
==================================================
Val Loss (MSE): 0.002456
Val MAE: 0.0389
Val RMSE: 0.0496

Plots saved to checkpoints/

Training complete!
```

---

## 6. 导出 ONNX

### 6.1 导出脚本

```python
# training/bandwidth/export_onnx.py

import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from model import SimpleLSTMPredictor, LSTMBandwidthPredictor


def load_config(config_path: str):
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config):
    model_type = config['model']['type']

    if model_type == 'simple':
        return SimpleLSTMPredictor(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model']['hidden_dim'],
            num_layers=config['model']['num_layers']
        )
    elif model_type == 'full':
        return LSTMBandwidthPredictor(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model']['hidden_dim'],
            num_layers=config['model']['num_layers']
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def export_onnx(model, history_length: int, input_dim: int, output_path: str):
    """导出模型到 ONNX 格式"""
    model.eval()

    # 创建示例输入 [batch=1, seq_len, input_dim]
    dummy_input = torch.randn(1, history_length, input_dim)

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

    # 添加 metadata
    onnx_model = onnx.load(output_path)

    # 嵌入 history_length 到 metadata
    onnx_model.metadata_props.append(
        onnx.StringStringEntryProto(key="history_length", value=str(history_length))
    )

    onnx.save(onnx_model, output_path)
    print(f"Embedded history_length={history_length} into model metadata")

    # 验证
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")

    return output_path


def verify_onnx(pytorch_model, onnx_path, history_length: int, input_dim: int):
    """验证 ONNX 模型与 PyTorch 模型输出一致"""
    pytorch_model.eval()

    # 创建测试输入
    test_input = np.random.randn(5, history_length, input_dim).astype(np.float32)

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
    parser = argparse.ArgumentParser(description='Export bandwidth model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/best_model.pth')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 创建并加载模型
    model = create_model(config)
    model.load_state_dict(torch.load(args.checkpoint, map_location='cpu', weights_only=True))
    model.eval()
    print(f"Loaded model from {args.checkpoint}")

    # 导出 ONNX
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    history_length = config['data']['history_length']
    input_dim = config['model']['input_dim']

    export_onnx(model, history_length, input_dim, output_path)

    # 验证
    verify_onnx(model, output_path, history_length, input_dim)

    # 打印模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}")
    print(f"Output: {onnx_model.graph.output[0].name}")
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")

    # 打印 metadata
    if onnx_model.metadata_props:
        print("\nMetadata:")
        for prop in onnx_model.metadata_props:
            print(f"  {prop.key}: {prop.value}")


if __name__ == '__main__':
    main()
```

### 6.2 运行导出

```bash
python export_onnx.py --config config.yaml
```

---

## 7. C++ 集成

### 7.1 更新 BandwidthPredictor 读取 metadata

与异常检测类似，从模型 metadata 读取 `history_length`：

```cpp
// src/ai/bandwidth_model.cpp

BandwidthPredictor::BandwidthPredictor(
    const std::string& model_path,
    size_t history_length,
    uint32_t max_bandwidth
)
    : _history_length(history_length)
    , _max_bandwidth(max_bandwidth)
{
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // 尝试从 metadata 读取 history_length
        auto hl_str = _inference->get_metadata("history_length");
        if (hl_str) {
            try {
                _history_length = std::stoul(*hl_str);
                LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu (from metadata)",
                         _history_length);
            } catch (...) {
                LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu (from parameter)",
                         _history_length);
            }
        } else {
            LOG_INFO(AI, "Bandwidth predictor loaded, history_length=%zu, max_bw=%u",
                     _history_length, _max_bandwidth);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load bandwidth model: %s", e.what());
        _inference = nullptr;
    }
}
```

### 7.2 输入格式说明

C++ 侧 `BandwidthPredictor::infer()` 需要将输入展平为一维数组：

```cpp
// 输入格式: [throughput_0, ..., throughput_N, rtt_0, ..., rtt_N, loss_0, ..., loss_N]
// 共 history_length * 3 个元素

// 注意: ONNX 模型期望的是 [batch, seq_len, features]
// 但我们的 ONNXInference 期望一维输入
// 需要确保输入顺序与训练一致
```

**重要**：当前 `bandwidth_model.cpp` 的输入格式可能与 ONNX 模型不匹配，需要根据实际情况调整。

### 7.3 与 Orca 拥塞控制的整合

带宽预测的结果会被缓存，并作为 Orca 拥塞控制模型的第 7 维输入：

```cpp
// include/neustack/ai/ai_model.hpp
// Orca 输入现在是 7 维
struct Input {
    float throughput_normalized;
    float queuing_delay_normalized;
    float rtt_ratio;
    float loss_rate;
    float cwnd_normalized;
    float in_flight_ratio;
    float predicted_bw_normalized;  // ← 来自带宽预测
};
```

`IntelligencePlane` 中的协作流程：

```
带宽预测 (100ms)  ──缓存──►  _cached_predicted_bw
                                      │
Orca (10ms)       ◄──读取─────────────┘
```

- `process_bandwidth()` 将预测结果归一化后存入 `_cached_predicted_bw`
- `process_orca()` 每次推理时读取缓存值作为额外输入
- 带宽预测频率 (100ms) 低于 Orca (10ms)，所以 Orca 多次推理会复用同一个预测值

这意味着 **Orca 模型训练时输入维度需要从 6 改为 7**，详见教程 06。

---

## 8. 完整训练流程

```bash
# 1. 创建目录和文件
mkdir -p training/bandwidth
# 将 dataset.py, model.py, train.py, export_onnx.py, config.yaml 放入

# 2. 训练
cd training/bandwidth
python train.py --config config.yaml

# 3. 导出
python export_onnx.py --config config.yaml

# 4. 测试
cd ../../build
./examples/ai_test
```

---

## 9. 新增文件清单

```
training/bandwidth/
├── config.yaml
├── dataset.py
├── model.py
├── train.py
└── export_onnx.py

models/
└── bandwidth_predictor.onnx  # 训练后的模型
```

---

## 10. 下一步

- **教程 06: Orca 拥塞控制训练** — DDPG 强化学习

---

## 11. 参考资料

- [LSTM Networks for Time Series](https://colah.github.io/posts/2015-08-Understanding-LSTMs/)
- [Network Traffic Prediction with LSTM](https://arxiv.org/abs/1912.04755)
- [PyTorch LSTM Documentation](https://pytorch.org/docs/stable/generated/torch.nn.LSTM.html)
