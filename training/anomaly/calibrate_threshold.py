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