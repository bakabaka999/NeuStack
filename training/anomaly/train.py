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
