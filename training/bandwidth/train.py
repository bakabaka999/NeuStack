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
