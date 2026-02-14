"""
training/security/train.py

安全异常检测模型训练

与 training/anomaly/train.py 风格一致, 增强点:
  1. 利用标签数据进行正式的 Precision/Recall/F1 评估
  2. Cosine Annealing LR 调度 (比 ReduceLROnPlateau 更稳定)
  3. F1 最优阈值搜索 (有标签时) 或 percentile (无标签时)
  4. 合成异常严格评估 (SYN Flood / Port Scan / DDoS / Slowloris / Amplification)

用法:
    # 合成数据训练
    cd training/security && python train.py

    # 真实数据训练
    cd training/security && python train.py --data ../real_data/security_dataset.npz
"""

import os
import yaml
import argparse
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from typing import Dict, Tuple

from model import DeepAutoencoder, SimpleAutoencoder
from dataset import (
    create_dataloaders,
    create_dataloaders_from_npz,
    SyntheticSecurityDataset,
)


# ============================================================================
# 配置加载 & 模型创建
# ============================================================================

def load_config(config_path: str) -> Dict:
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config: Dict) -> nn.Module:
    model_type = config['model']['type']

    if model_type == 'deep':
        return DeepAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model']['hidden_dims'],
            latent_dim=config['model']['latent_dim'],
            dropout=config['model'].get('dropout', 0.1),
        )
    elif model_type == 'simple':
        return SimpleAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model'].get('simple_hidden_dims', [32, 16]),
            latent_dim=config['model'].get('simple_latent_dim', 8),
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


# ============================================================================
# 训练 & 验证
# ============================================================================

def train_epoch(
    model: nn.Module,
    loader: DataLoader,
    optimizer: optim.Optimizer,
    device: torch.device,
) -> float:
    model.train()
    total_loss = 0.0

    for batch, _ in loader:
        batch = batch.to(device)
        reconstructed = model(batch)
        loss = nn.functional.mse_loss(reconstructed, batch)

        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item() * batch.size(0)

    return total_loss / len(loader.dataset)


def validate(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
) -> Tuple[float, np.ndarray, np.ndarray]:
    """验证, 返回 (avg_loss, 逐样本误差, 标签)"""
    model.eval()
    total_loss = 0.0
    all_errors = []
    all_labels = []

    with torch.no_grad():
        for batch, labels in loader:
            batch = batch.to(device)
            reconstructed = model(batch)
            errors = torch.mean((batch - reconstructed) ** 2, dim=1)

            total_loss += errors.sum().item()
            all_errors.extend(errors.cpu().numpy())
            all_labels.extend(labels.numpy())

    return (
        total_loss / len(loader.dataset),
        np.array(all_errors),
        np.array(all_labels),
    )


# ============================================================================
# 阈值搜索 & 评估
# ============================================================================

def find_threshold_percentile(errors: np.ndarray, labels: np.ndarray, percentile: float = 95) -> float:
    """正常样本误差的 percentile 分位数"""
    normal_errors = errors[labels == 0]
    if len(normal_errors) == 0:
        return float(np.percentile(errors, percentile))
    return float(np.percentile(normal_errors, percentile))


def find_threshold_f1_optimal(errors: np.ndarray, labels: np.ndarray) -> float:
    """
    搜索使 F1 最大的阈值

    在误差分布的 [P50, P99.5] 范围内等间距搜索 200 个候选阈值
    """
    if np.sum(labels == 1) == 0:
        # 没有异常样本, fallback 到 percentile
        return find_threshold_percentile(errors, labels, 95)

    candidates = np.linspace(
        np.percentile(errors, 50),
        np.percentile(errors, 99.5),
        200,
    )

    best_f1 = -1
    best_threshold = candidates[0]

    for t in candidates:
        preds = (errors > t).astype(int)
        tp = np.sum((preds == 1) & (labels == 1))
        fp = np.sum((preds == 1) & (labels == 0))
        fn = np.sum((preds == 0) & (labels == 1))

        precision = tp / (tp + fp) if (tp + fp) > 0 else 0
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

        if f1 > best_f1:
            best_f1 = f1
            best_threshold = t

    return float(best_threshold)


def evaluate_detection(
    errors: np.ndarray,
    labels: np.ndarray,
    threshold: float,
) -> Dict:
    """分类指标"""
    predictions = (errors > threshold).astype(int)

    tp = int(np.sum((predictions == 1) & (labels == 1)))
    tn = int(np.sum((predictions == 0) & (labels == 0)))
    fp = int(np.sum((predictions == 1) & (labels == 0)))
    fn = int(np.sum((predictions == 0) & (labels == 1)))

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
        'tp': tp, 'tn': tn, 'fp': fp, 'fn': fn,
    }


def evaluate_with_synthetic_anomalies(
    model: nn.Module,
    normal_data: np.ndarray,
    device: torch.device,
    threshold: float,
    n_anomalies: int = 1000,
    seed: int = 123,
) -> Dict:
    """
    合成异常严格评估

    5 种攻击类型: SYN Flood, Port Scan, DDoS, Slowloris, Amplification
    """
    # 生成合成异常
    anomaly_dataset = SyntheticSecurityDataset(
        n_samples=n_anomalies,
        anomaly_ratio=1.0,
        seed=seed,
    )
    anomaly_data = anomaly_dataset.data

    # 采样正常数据
    n_normal = min(len(normal_data), n_anomalies)
    idx = np.random.choice(len(normal_data), n_normal, replace=False)
    normal_samples = normal_data[idx]

    all_data = np.vstack([normal_samples, anomaly_data])
    all_labels = np.concatenate([
        np.zeros(n_normal),
        np.ones(n_anomalies),
    ])

    # 推理
    model.eval()
    with torch.no_grad():
        tensor = torch.FloatTensor(all_data).to(device)
        reconstructed = model(tensor)
        errors = torch.mean((tensor - reconstructed) ** 2, dim=1).cpu().numpy()

    metrics = evaluate_detection(errors, all_labels, threshold)

    # 按攻击类型分析
    n_per_type = n_anomalies // 5
    attack_names = ['SYN Flood', 'Port Scan', 'DDoS Flood', 'Slowloris', 'Amplification']

    type_recalls = {}
    for i, name in enumerate(attack_names):
        start = n_normal + i * n_per_type
        end = start + n_per_type
        type_errors = errors[start:end]
        detected = np.sum(type_errors > threshold)
        type_recalls[name] = detected / n_per_type

    metrics['attack_recalls'] = type_recalls
    metrics['n_normal'] = n_normal
    metrics['n_anomaly'] = n_anomalies

    return metrics


# ============================================================================
# 可视化
# ============================================================================

def plot_training_history(train_losses: list, val_losses: list, save_path: str):
    plt.figure(figsize=(10, 5))
    plt.plot(train_losses, label='Train Loss')
    plt.plot(val_losses, label='Val Loss')
    plt.xlabel('Epoch')
    plt.ylabel('MSE Loss')
    plt.title('Security Anomaly Detection — Training History')
    plt.legend()
    plt.grid(True)
    plt.savefig(save_path, dpi=150)
    plt.close()


def plot_error_distribution(
    errors: np.ndarray,
    labels: np.ndarray,
    threshold: float,
    save_path: str,
):
    plt.figure(figsize=(10, 5))

    normal_errors = errors[labels == 0]
    anomaly_errors = errors[labels == 1]

    plt.hist(normal_errors, bins=60, alpha=0.7, label='Normal', color='steelblue')
    if len(anomaly_errors) > 0:
        plt.hist(anomaly_errors, bins=60, alpha=0.7, label='Anomaly', color='crimson')

    plt.axvline(x=threshold, color='green', linestyle='--', linewidth=2,
                label=f'Threshold={threshold:.6f}')

    plt.xlabel('Reconstruction Error (MSE)')
    plt.ylabel('Count')
    plt.title('Security Model — Error Distribution')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.savefig(save_path, dpi=150)
    plt.close()


def plot_latent_space(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    save_path: str,
):
    """可视化 latent space (仅对 DeepAutoencoder)"""
    if not hasattr(model, 'encode'):
        return

    model.eval()
    all_latent = []
    all_labels = []

    with torch.no_grad():
        for batch, labels in loader:
            batch = batch.to(device)
            latent = model.encode(batch)
            all_latent.append(latent.cpu().numpy())
            all_labels.extend(labels.numpy())

    all_latent = np.vstack(all_latent)
    all_labels = np.array(all_labels)

    if all_latent.shape[1] < 2:
        return

    plt.figure(figsize=(8, 6))
    normal_mask = all_labels == 0
    anomaly_mask = all_labels == 1

    plt.scatter(all_latent[normal_mask, 0], all_latent[normal_mask, 1],
                alpha=0.3, s=5, c='steelblue', label='Normal')
    if anomaly_mask.any():
        plt.scatter(all_latent[anomaly_mask, 0], all_latent[anomaly_mask, 1],
                    alpha=0.5, s=10, c='crimson', label='Anomaly')

    plt.xlabel('Latent Dim 0')
    plt.ylabel('Latent Dim 1')
    plt.title('Latent Space (first 2 dims)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.savefig(save_path, dpi=150)
    plt.close()


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='Train security anomaly detection model')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--data', type=str, default=None,
                        help='Path to security_dataset.npz (overrides synthetic data)')
    args = parser.parse_args()

    config = load_config(args.config)
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    os.makedirs(config['output']['model_dir'], exist_ok=True)

    # ─── 数据加载 ───
    test_loader = None

    if args.data:
        print(f"\nLoading real data from: {args.data}")
        loaders = create_dataloaders_from_npz(
            npz_path=args.data,
            train_ratio=config['data']['train_ratio'],
            batch_size=config['training']['batch_size'],
            seed=config['data']['seed'],
        )
        train_loader = loaders['train']
        val_loader = loaders['val']
        test_loader = loaders['test']
    else:
        print("\nUsing synthetic data")
        train_loader, val_loader = create_dataloaders(
            train_samples=config['data']['train_samples'],
            val_samples=config['data']['val_samples'],
            batch_size=config['training']['batch_size'],
            seed=config['data']['seed'],
        )

    print(f"Train samples: {len(train_loader.dataset)}")
    print(f"Val samples:   {len(val_loader.dataset)}")
    if test_loader:
        print(f"Test samples:  {len(test_loader.dataset)}")

    # ─── 模型 ───
    model = create_model(config).to(device)
    print(f"\nModel: {type(model).__name__}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    # ─── 优化器 & 调度器 ───
    optimizer = optim.Adam(
        model.parameters(),
        lr=config['training']['learning_rate'],
        weight_decay=config['training']['weight_decay'],
    )

    scheduler_type = config['training'].get('scheduler', 'cosine')
    if scheduler_type == 'cosine':
        scheduler = optim.lr_scheduler.CosineAnnealingLR(
            optimizer,
            T_max=config['training'].get('scheduler_T_max', config['training']['epochs']),
            eta_min=config['training'].get('scheduler_eta_min', 1e-5),
        )
    else:
        scheduler = optim.lr_scheduler.ReduceLROnPlateau(
            optimizer, mode='min', factor=0.5, patience=5,
        )

    # ─── 训练循环 ───
    train_losses = []
    val_losses = []
    best_val_loss = float('inf')
    patience_counter = 0
    epochs = config['training']['epochs']
    patience = config['training']['early_stopping_patience']

    print(f"\nTraining for up to {epochs} epochs (patience={patience})...\n")

    for epoch in range(epochs):
        train_loss = train_epoch(model, train_loader, optimizer, device)
        train_losses.append(train_loss)

        val_loss, _, _ = validate(model, val_loader, device)
        val_losses.append(val_loss)

        # 调度器 step
        if scheduler_type == 'cosine':
            scheduler.step()
        else:
            scheduler.step(val_loss)

        current_lr = optimizer.param_groups[0]['lr']

        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"Epoch {epoch+1:3d}/{epochs}: "
                  f"Train={train_loss:.6f}  Val={val_loss:.6f}  LR={current_lr:.2e}")

        # Early stopping
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            patience_counter = 0
            torch.save(
                model.state_dict(),
                os.path.join(config['output']['model_dir'], 'best_model.pth'),
            )
        else:
            patience_counter += 1
            if patience_counter >= patience:
                print(f"\nEarly stopping at epoch {epoch+1}")
                break

    # ─── 加载最佳模型 ───
    model.load_state_dict(
        torch.load(
            os.path.join(config['output']['model_dir'], 'best_model.pth'),
            map_location=device,
            weights_only=True,
        )
    )

    # ─── 阈值搜索 ───
    print("\n" + "=" * 60)
    print("Threshold Calibration")
    print("=" * 60)

    # 在测试集或验证集上搜索阈值
    eval_loader = test_loader if test_loader else val_loader
    _, eval_errors, eval_labels = validate(model, eval_loader, device)

    threshold_method = config.get('threshold', {}).get('method', 'f1_optimal')

    if threshold_method == 'f1_optimal' and np.sum(eval_labels == 1) > 0:
        threshold = find_threshold_f1_optimal(eval_errors, eval_labels)
        print(f"Threshold (F1-optimal): {threshold:.6f}")
    else:
        pct = config.get('threshold', {}).get('percentile', 95)
        threshold = find_threshold_percentile(eval_errors, eval_labels, pct)
        print(f"Threshold (P{pct} of normal errors): {threshold:.6f}")

    # ─── 评估 ───
    print("\n" + "=" * 60)
    print("Evaluation on labeled data")
    print("=" * 60)

    metrics = evaluate_detection(eval_errors, eval_labels, threshold)
    print(f"Accuracy:  {metrics['accuracy']:.4f}")
    print(f"Precision: {metrics['precision']:.4f}")
    print(f"Recall:    {metrics['recall']:.4f}")
    print(f"F1 Score:  {metrics['f1']:.4f}")
    print(f"  TP={metrics['tp']}  FP={metrics['fp']}  FN={metrics['fn']}  TN={metrics['tn']}")

    # ─── 合成异常评估 ───
    print("\n" + "=" * 60)
    print("Synthetic Anomaly Evaluation")
    print("=" * 60)

    all_normal = []
    for batch, _ in train_loader:
        all_normal.append(batch.numpy())
    all_normal = np.vstack(all_normal)

    synth_metrics = evaluate_with_synthetic_anomalies(
        model, all_normal, device, threshold, n_anomalies=1000,
    )

    print(f"Test: {synth_metrics['n_normal']} normal + {synth_metrics['n_anomaly']} anomalies")
    print(f"  Accuracy:  {synth_metrics['accuracy']:.4f}  |  F1: {synth_metrics['f1']:.4f}")
    print(f"  Precision: {synth_metrics['precision']:.4f}  |  Recall: {synth_metrics['recall']:.4f}")
    print(f"  Detection by attack type:")
    for name, recall in synth_metrics['attack_recalls'].items():
        status = "✓" if recall > 0.8 else ("△" if recall > 0.5 else "✗")
        print(f"    {status} {name:15s}: {recall*100:5.1f}%")

    print(f"\n  Confusion Matrix:")
    print(f"    TP: {synth_metrics['tp']:4d}  |  FP: {synth_metrics['fp']:4d}")
    print(f"    FN: {synth_metrics['fn']:4d}  |  TN: {synth_metrics['tn']:4d}")

    # ─── 可视化 ───
    plot_training_history(
        train_losses, val_losses,
        os.path.join(config['output']['model_dir'], 'training_history.png'),
    )

    plot_error_distribution(
        eval_errors, eval_labels, threshold,
        os.path.join(config['output']['model_dir'], 'error_distribution.png'),
    )

    if test_loader:
        plot_latent_space(
            model, test_loader, device,
            os.path.join(config['output']['model_dir'], 'latent_space.png'),
        )

    # ─── 保存阈值 ───
    threshold_path = os.path.join(config['output']['model_dir'], 'threshold.txt')
    with open(threshold_path, 'w') as f:
        f.write(f"{threshold:.6f}\n")

    print(f"\nThreshold saved to {threshold_path}")
    print(f"Plots saved to {config['output']['model_dir']}/")
    print("\nTraining complete!")


if __name__ == '__main__':
    main()
