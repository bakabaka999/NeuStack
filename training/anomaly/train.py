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
from dataset import create_dataloaders, create_dataloaders_from_npz


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
        'threshold': threshold,
        'tp': int(tp),
        'tn': int(tn),
        'fp': int(fp),
        'fn': int(fn),
    }


def generate_synthetic_anomalies(n_samples: int, seed: int = 42, difficulty: str = 'hard') -> Tuple[np.ndarray, np.ndarray]:
    """
    生成多种类型的合成异常数据用于严格评估

    特征顺序: [syn_rate, rst_rate, new_conn_rate, packet_rate, avg_packet_size]
    所有值归一化到 [0, 1]

    difficulty:
      - 'easy': 明显异常 (原来的方式)
      - 'hard': 边界异常，更接近正常数据，更难检测
    """
    np.random.seed(seed)

    n_per_type = n_samples // 5
    anomalies = []

    if difficulty == 'easy':
        # === 明显异常 (原来的方式) ===

        # SYN Flood
        syn_flood = np.column_stack([
            np.random.uniform(0.7, 1.0, n_per_type),
            np.random.uniform(0.0, 0.1, n_per_type),
            np.random.uniform(0.6, 1.0, n_per_type),
            np.random.uniform(0.5, 0.9, n_per_type),
            np.random.uniform(0.02, 0.08, n_per_type),
        ])
        anomalies.append(syn_flood)

        # Port Scan
        port_scan = np.column_stack([
            np.random.uniform(0.4, 0.8, n_per_type),
            np.random.uniform(0.6, 1.0, n_per_type),
            np.random.uniform(0.3, 0.7, n_per_type),
            np.random.uniform(0.3, 0.6, n_per_type),
            np.random.uniform(0.02, 0.06, n_per_type),
        ])
        anomalies.append(port_scan)

        # DDoS Flood
        ddos_flood = np.column_stack([
            np.random.uniform(0.1, 0.4, n_per_type),
            np.random.uniform(0.0, 0.2, n_per_type),
            np.random.uniform(0.1, 0.3, n_per_type),
            np.random.uniform(0.85, 1.0, n_per_type),
            np.random.uniform(0.3, 0.8, n_per_type),
        ])
        anomalies.append(ddos_flood)

        # Slowloris
        slowloris = np.column_stack([
            np.random.uniform(0.3, 0.6, n_per_type),
            np.random.uniform(0.0, 0.05, n_per_type),
            np.random.uniform(0.5, 0.9, n_per_type),
            np.random.uniform(0.01, 0.1, n_per_type),
            np.random.uniform(0.01, 0.05, n_per_type),
        ])
        anomalies.append(slowloris)

        # Large Packet
        large_packet = np.column_stack([
            np.random.uniform(0.05, 0.2, n_per_type),
            np.random.uniform(0.0, 0.1, n_per_type),
            np.random.uniform(0.05, 0.2, n_per_type),
            np.random.uniform(0.1, 0.4, n_per_type),
            np.random.uniform(0.9, 1.0, n_per_type),
        ])
        anomalies.append(large_packet)

    else:
        # === 边界异常 (更难检测) ===
        # 基于真实数据分布：大部分值接近 0，异常在边界处

        # 1. 轻微 SYN 异常 - 只比正常稍高
        syn_mild = np.column_stack([
            np.random.uniform(0.01, 0.08, n_per_type),   # syn_rate: 比 p95(0) 稍高
            np.random.uniform(0.0, 0.01, n_per_type),    # rst_rate: 接近 0
            np.random.uniform(0.01, 0.05, n_per_type),   # new_conn_rate: 稍高
            np.random.uniform(0.02, 0.05, n_per_type),   # packet_rate: 正常偏高
            np.random.uniform(0.05, 0.15, n_per_type),   # avg_pkt_size: 正常偏小
        ])
        anomalies.append(syn_mild)

        # 2. RST 异常 - 正常数据 RST=0，任何 RST>0 都是异常
        rst_anomaly = np.column_stack([
            np.random.uniform(0.0, 0.02, n_per_type),    # syn_rate: 正常
            np.random.uniform(0.01, 0.1, n_per_type),    # rst_rate: >0 就是异常！
            np.random.uniform(0.0, 0.02, n_per_type),    # new_conn_rate: 正常
            np.random.uniform(0.0, 0.03, n_per_type),    # packet_rate: 正常
            np.random.uniform(0.1, 0.5, n_per_type),     # avg_pkt_size: 正常
        ])
        anomalies.append(rst_anomaly)

        # 3. 流量突增 - packet_rate 异常高
        traffic_spike = np.column_stack([
            np.random.uniform(0.0, 0.01, n_per_type),    # syn_rate: 正常
            np.random.uniform(0.0, 0.005, n_per_type),   # rst_rate: 接近 0
            np.random.uniform(0.0, 0.01, n_per_type),    # new_conn_rate: 正常
            np.random.uniform(0.08, 0.2, n_per_type),    # packet_rate: 超过正常 max(0.078)
            np.random.uniform(0.1, 0.4, n_per_type),     # avg_pkt_size: 正常
        ])
        anomalies.append(traffic_spike)

        # 4. 连接数异常 - new_conn_rate 异常高
        conn_anomaly = np.column_stack([
            np.random.uniform(0.005, 0.03, n_per_type),  # syn_rate: 稍高
            np.random.uniform(0.0, 0.005, n_per_type),   # rst_rate: 接近 0
            np.random.uniform(0.05, 0.15, n_per_type),   # new_conn_rate: 异常高
            np.random.uniform(0.01, 0.04, n_per_type),   # packet_rate: 正常
            np.random.uniform(0.1, 0.3, n_per_type),     # avg_pkt_size: 正常
        ])
        anomalies.append(conn_anomaly)

        # 5. 混合轻微异常 - 多个特征都稍微偏离
        mixed_mild = np.column_stack([
            np.random.uniform(0.005, 0.02, n_per_type),  # syn_rate: 稍高
            np.random.uniform(0.005, 0.02, n_per_type),  # rst_rate: 稍高
            np.random.uniform(0.005, 0.02, n_per_type),  # new_conn_rate: 稍高
            np.random.uniform(0.03, 0.06, n_per_type),   # packet_rate: 稍高
            np.random.uniform(0.05, 0.15, n_per_type),   # avg_pkt_size: 偏小
        ])
        anomalies.append(mixed_mild)

    all_anomalies = np.vstack(anomalies).astype(np.float32)
    labels = np.ones(len(all_anomalies))

    return all_anomalies, labels


def evaluate_with_synthetic_anomalies(
    model: nn.Module,
    normal_data: np.ndarray,
    device: torch.device,
    threshold: float,
    n_anomalies: int = 1000,
    difficulty: str = 'hard'
) -> Dict:
    """使用合成异常数据进行严格评估"""

    # 生成合成异常
    anomaly_data, anomaly_labels = generate_synthetic_anomalies(n_anomalies, difficulty=difficulty)

    # 合并正常数据和异常数据
    # 从正常数据中采样相同数量
    n_normal = min(len(normal_data), n_anomalies)
    normal_indices = np.random.choice(len(normal_data), n_normal, replace=False)
    normal_samples = normal_data[normal_indices]
    normal_labels = np.zeros(n_normal)

    all_data = np.vstack([normal_samples, anomaly_data])
    all_labels = np.concatenate([normal_labels, anomaly_labels])

    # 计算重构误差
    model.eval()
    with torch.no_grad():
        data_tensor = torch.FloatTensor(all_data).to(device)
        reconstructed = model(data_tensor)
        errors = torch.mean((data_tensor - reconstructed) ** 2, dim=1).cpu().numpy()

    # 评估
    metrics = evaluate_detection(errors, all_labels, threshold)

    # 按攻击类型分析
    n_per_type = n_anomalies // 5
    if difficulty == 'easy':
        attack_types = ['SYN Flood', 'Port Scan', 'DDoS Flood', 'Slowloris', 'Large Packet']
    else:
        attack_types = ['Mild SYN', 'RST Anomaly', 'Traffic Spike', 'Conn Anomaly', 'Mixed Mild']

    type_recalls = {}

    for i, attack_name in enumerate(attack_types):
        start_idx = n_normal + i * n_per_type
        end_idx = start_idx + n_per_type
        type_errors = errors[start_idx:end_idx]
        detected = np.sum(type_errors > threshold)
        type_recalls[attack_name] = detected / n_per_type

    metrics['attack_recalls'] = type_recalls
    metrics['n_normal'] = n_normal
    metrics['n_anomaly'] = n_anomalies
    metrics['difficulty'] = difficulty

    return metrics


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
    parser.add_argument('--data', type=str, default=None,
                        help='Path to npz data file (overrides synthetic data)')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 设备
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # 创建输出目录
    os.makedirs(config['output']['model_dir'], exist_ok=True)

    # 创建数据加载器
    if args.data:
        # 使用真实数据
        print(f"Loading real data from: {args.data}")
        train_loader, val_loader = create_dataloaders_from_npz(
            npz_path=args.data,
            train_ratio=0.8,
            batch_size=config['training']['batch_size'],
            seed=config['data']['seed']
        )
    else:
        # 使用合成数据
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

    # 评估检测性能 (原始数据)
    metrics = evaluate_detection(val_errors, val_labels, threshold)
    print(f"\n[Real Data Only]")
    print(f"Accuracy:  {metrics['accuracy']:.4f}")
    print(f"Precision: {metrics['precision']:.4f}")
    print(f"Recall:    {metrics['recall']:.4f}")
    print(f"F1 Score:  {metrics['f1']:.4f}")

    # === 合成异常严格评估 ===
    print("\n" + "=" * 50)
    print("Strict Evaluation with Synthetic Anomalies")
    print("=" * 50)

    # 获取所有训练数据作为正常数据基准
    all_normal_data = []
    for batch, _ in train_loader:
        all_normal_data.append(batch.numpy())
    all_normal_data = np.vstack(all_normal_data)

    # === Easy 模式 (明显异常) ===
    print("\n[EASY MODE - Obvious Anomalies]")
    easy_metrics = evaluate_with_synthetic_anomalies(
        model, all_normal_data, device, threshold, n_anomalies=1000, difficulty='easy'
    )

    print(f"Test Set: {easy_metrics['n_normal']} normal + {easy_metrics['n_anomaly']} anomalies")
    print(f"  Accuracy:  {easy_metrics['accuracy']:.4f}  |  F1: {easy_metrics['f1']:.4f}")
    print(f"  Precision: {easy_metrics['precision']:.4f}  |  Recall: {easy_metrics['recall']:.4f}")
    print(f"  Detection by type:")
    for attack_name, recall in easy_metrics['attack_recalls'].items():
        status = "✓" if recall > 0.8 else ("△" if recall > 0.5 else "✗")
        print(f"    {status} {attack_name:15s}: {recall*100:5.1f}%")

    # === Hard 模式 (边界异常) ===
    print("\n[HARD MODE - Subtle Anomalies]")
    hard_metrics = evaluate_with_synthetic_anomalies(
        model, all_normal_data, device, threshold, n_anomalies=1000, difficulty='hard'
    )

    print(f"Test Set: {hard_metrics['n_normal']} normal + {hard_metrics['n_anomaly']} anomalies")
    print(f"  Accuracy:  {hard_metrics['accuracy']:.4f}  |  F1: {hard_metrics['f1']:.4f}")
    print(f"  Precision: {hard_metrics['precision']:.4f}  |  Recall: {hard_metrics['recall']:.4f}")
    print(f"  Detection by type:")
    for attack_name, recall in hard_metrics['attack_recalls'].items():
        status = "✓" if recall > 0.8 else ("△" if recall > 0.5 else "✗")
        print(f"    {status} {attack_name:15s}: {recall*100:5.1f}%")

    print(f"\nConfusion Matrix (Hard Mode):")
    print(f"  TP: {hard_metrics['tp']:4d}  |  FP: {hard_metrics['fp']:4d}")
    print(f"  FN: {hard_metrics['fn']:4d}  |  TN: {hard_metrics['tn']:4d}")

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
