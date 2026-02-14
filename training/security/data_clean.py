"""
training/security/data_clean.py

数据质量分析 + 清洗

用法:
    cd training/security
    python data_clean.py --data ../real_data/security_dataset.npz

功能:
  1. 数据分布分析 (找出正常/异常重叠区域)
  2. 标签一致性检查 (找可疑标签)
  3. 离群点检测 (正常数据中的异常 & 异常数据中的正常)
  4. 输出清洗后的数据集
"""

import argparse
import numpy as np
from pathlib import Path

FEATURE_NAMES = [
    'pps_norm', 'bps_norm', 'syn_rate_norm', 'rst_rate_norm',
    'syn_ratio_norm', 'new_conn_rate_norm', 'avg_pkt_size_norm', 'rst_ratio_norm',
]


def analyze_data(inputs: np.ndarray, labels: np.ndarray):
    """全面数据分析"""
    normal = inputs[labels == 0]
    anomaly = inputs[labels == 1]

    print("=" * 70)
    print("数据概览")
    print("=" * 70)
    print(f"  总样本: {len(inputs)}")
    print(f"  正常:   {len(normal)} ({len(normal)/len(inputs):.1%})")
    print(f"  异常:   {len(anomaly)} ({len(anomaly)/len(inputs):.1%})")
    print(f"  特征数: {inputs.shape[1]}")
    print(f"  NaN:    {np.isnan(inputs).sum()}")
    print(f"  Inf:    {np.isinf(inputs).sum()}")
    print(f"  超出[0,1]: {np.sum((inputs < 0) | (inputs > 1))}")

    print("\n" + "=" * 70)
    print("特征分布对比")
    print("=" * 70)
    print(f"  {'Feature':<20s} {'Normal mean±std':<20s} {'Anomaly mean±std':<20s} {'Separability':>12s}")
    print("  " + "-" * 66)

    separabilities = []
    for i, name in enumerate(FEATURE_NAMES):
        n_col = normal[:, i]
        a_col = anomaly[:, i]
        n_mean, n_std = n_col.mean(), n_col.std()
        a_mean, a_std = a_col.mean(), a_col.std()

        # Cohen's d (效应量)
        pooled_std = np.sqrt((n_std**2 + a_std**2) / 2) if (n_std + a_std) > 0 else 1e-8
        cohens_d = abs(n_mean - a_mean) / pooled_std
        separabilities.append((name, cohens_d))

        sep_label = "⚠ LOW" if cohens_d < 0.5 else ("OK" if cohens_d < 1.0 else "✓ GOOD")
        print(f"  {name:<20s} {n_mean:.4f}±{n_std:.4f}      {a_mean:.4f}±{a_std:.4f}      d={cohens_d:.2f} {sep_label}")

    print("\n  可分性排序 (Cohen's d, 越大越好):")
    for name, d in sorted(separabilities, key=lambda x: x[1], reverse=True):
        print(f"    {name:<20s}: {d:.3f}")

    return normal, anomaly


def find_suspicious_labels(inputs: np.ndarray, labels: np.ndarray, contamination: float = 0.05):
    """
    用 Isolation Forest 思路找可疑标签

    简化版: 用 Mahalanobis 距离
    - 正常样本中距正常中心太远的 → 可能是错标的异常
    - 异常样本中距正常中心太近的 → 可能是错标的正常
    """
    normal = inputs[labels == 0]
    anomaly = inputs[labels == 1]

    # 正常数据的统计量
    mean = normal.mean(axis=0)
    std = normal.std(axis=0) + 1e-8

    # Z-score 距离
    normal_z = np.sqrt(np.sum(((normal - mean) / std) ** 2, axis=1))
    anomaly_z = np.sqrt(np.sum(((anomaly - mean) / std) ** 2, axis=1))

    print("\n" + "=" * 70)
    print("标签一致性分析")
    print("=" * 70)

    # 正常数据中的异常 (Z 距离 > P97.5)
    normal_threshold = np.percentile(normal_z, 97.5)
    suspect_normal = normal_z > normal_threshold
    print(f"\n  正常样本中可疑异常 (Z > P97.5={normal_threshold:.2f}):")
    print(f"    数量: {suspect_normal.sum()}/{len(normal)}")

    # 异常数据中的可疑正常 (Z 距离 < 正常 P50)
    normal_p50 = np.percentile(normal_z, 50)
    normal_p75 = np.percentile(normal_z, 75)
    suspect_anomaly_strict = anomaly_z < normal_p50
    suspect_anomaly_loose = anomaly_z < normal_p75

    print(f"\n  异常样本中可疑正常:")
    print(f"    Z < 正常 P50 ({normal_p50:.2f}): {suspect_anomaly_strict.sum()}/{len(anomaly)} ({suspect_anomaly_strict.sum()/len(anomaly):.1%})")
    print(f"    Z < 正常 P75 ({normal_p75:.2f}): {suspect_anomaly_loose.sum()}/{len(anomaly)} ({suspect_anomaly_loose.sum()/len(anomaly):.1%})")

    if suspect_anomaly_strict.sum() > 0:
        print(f"\n  这些 '异常' 样本的特征非常接近正常，可能是:")
        print(f"    - 标签错误 (实际是正常流量)")
        print(f"    - 极轻微的异常 (模型难以区分)")
        close_anom = anomaly[suspect_anomaly_strict]
        print(f"\n  可疑异常样本的特征均值:")
        for i, name in enumerate(FEATURE_NAMES):
            print(f"    {name:<20s}: {close_anom[:, i].mean():.4f} (正常均值: {mean[i]:.4f})")

    return {
        'normal_z': normal_z,
        'anomaly_z': anomaly_z,
        'suspect_normal_mask': suspect_normal,
        'suspect_anomaly_strict': suspect_anomaly_strict,
        'suspect_anomaly_loose': suspect_anomaly_loose,
        'normal_p50': normal_p50,
        'normal_p75': normal_p75,
    }


def check_duplicates(inputs: np.ndarray, labels: np.ndarray):
    """检查重复/近重复样本"""
    print("\n" + "=" * 70)
    print("重复样本分析")
    print("=" * 70)

    # 完全重复
    _, unique_idx, counts = np.unique(
        np.round(inputs, 6), axis=0, return_index=True, return_counts=True
    )
    n_dup = np.sum(counts > 1)
    total_dup = np.sum(counts[counts > 1]) - n_dup  # extra copies
    print(f"  完全重复组: {n_dup}")
    print(f"  多余副本: {total_dup}")

    # 近重复 (rounded to 3dp)
    rounded = np.round(inputs, 3)
    _, unique_idx_r, counts_r = np.unique(rounded, axis=0, return_index=True, return_counts=True)
    n_near_dup = np.sum(counts_r > 1)
    print(f"  近重复组 (3dp): {n_near_dup}")

    # 跨标签重复 (正常和异常有相同特征)
    normal = inputs[labels == 0]
    anomaly = inputs[labels == 1]
    normal_set = set(map(tuple, np.round(normal, 4)))
    cross_dup = 0
    for row in np.round(anomaly, 4):
        if tuple(row) in normal_set:
            cross_dup += 1
    print(f"  跨标签重复 (4dp): {cross_dup} (同时出现在正常和异常中)")

    if cross_dup > 0:
        print(f"  ⚠ 有 {cross_dup} 个异常样本与正常样本几乎相同 → 必须清洗!")


def clean_data(
    inputs: np.ndarray,
    labels: np.ndarray,
    analysis: dict,
    mode: str = 'moderate',
    idle_keep_ratio: float = 0.05,
) -> tuple:
    """
    清洗数据

    mode:
      - conservative: 只移除跨标签重复和极端可疑样本
      - moderate: 移除可疑异常(Z < 正常 P50) + 正常离群点
      - aggressive: 移除可疑异常(Z < 正常 P75) + 正常离群点

    idle_keep_ratio: 空闲样本保留比例 (相对于正常样本数, 默认 5%)
                     空闲样本 = 全零或近零行 (pps < 0.01)
                     保留这些样本让模型学到 "没流量 = 正常"
    """
    print(f"\n" + "=" * 70)
    print(f"数据清洗 (mode={mode})")
    print("=" * 70)

    normal_mask = labels == 0
    anomaly_mask = labels == 1

    # 识别空闲样本 (pps_norm 接近 0, 即第一个特征 < 0.01)
    # 这些样本必须保留，否则模型对全零输入重构误差过高导致误报
    idle_mask = (inputs[:, 0] < 0.01) & normal_mask
    n_idle = idle_mask.sum()
    n_normal = normal_mask.sum()
    min_idle_keep = max(10, int(n_normal * idle_keep_ratio))
    print(f"  空闲样本 (pps≈0): {n_idle}/{n_normal} 正常样本")
    print(f"  将保留至少 {min(n_idle, min_idle_keep)} 条空闲样本")

    keep = np.ones(len(inputs), dtype=bool)
    reasons = {}

    # 1. 移除 NaN/Inf
    bad_vals = np.isnan(inputs).any(axis=1) | np.isinf(inputs).any(axis=1)
    n_bad = bad_vals.sum()
    keep &= ~bad_vals
    if n_bad > 0:
        reasons['nan_inf'] = n_bad
        print(f"  移除 NaN/Inf: {n_bad}")

    # 2. 移除超出 [0,1] 范围的样本
    out_range = (inputs < -0.01).any(axis=1) | (inputs > 1.01).any(axis=1)
    n_out = out_range.sum()
    keep &= ~out_range
    if n_out > 0:
        reasons['out_of_range'] = n_out
        print(f"  移除超出范围: {n_out}")

    # 3. 移除跨标签重复
    normal_data = inputs[normal_mask]
    normal_set = set(map(tuple, np.round(normal_data, 4)))
    cross_dup_mask = np.zeros(len(inputs), dtype=bool)
    for i in range(len(inputs)):
        if labels[i] == 1 and tuple(np.round(inputs[i], 4)) in normal_set:
            cross_dup_mask[i] = True
    n_cross = cross_dup_mask.sum()
    keep &= ~cross_dup_mask
    if n_cross > 0:
        reasons['cross_label_dup'] = n_cross
        print(f"  移除跨标签重复异常: {n_cross}")

    # 4. 移除可疑异常样本 (与正常数据太接近)
    if mode in ('moderate', 'aggressive'):
        # 构建异常样本的全局索引到 anomaly_z 的映射
        anomaly_indices = np.where(anomaly_mask)[0]
        anomaly_z = analysis['anomaly_z']

        if mode == 'moderate':
            threshold = analysis['normal_p50']
        else:
            threshold = analysis['normal_p75']

        suspect_mask = np.zeros(len(inputs), dtype=bool)
        for local_i, global_i in enumerate(anomaly_indices):
            if anomaly_z[local_i] < threshold:
                suspect_mask[global_i] = True

        # 不要和已经移除的重复计算
        suspect_mask &= keep
        n_suspect = suspect_mask.sum()
        keep &= ~suspect_mask
        if n_suspect > 0:
            reasons[f'suspect_anomaly'] = n_suspect
            print(f"  移除可疑异常 (Z < {'P50' if mode=='moderate' else 'P75'}): {n_suspect}")

    # 5. 去重 (保留每组第一个, 但保证空闲样本不低于 min_idle_keep)
    rounded = np.round(inputs[keep], 6)
    kept_indices = np.where(keep)[0]
    _, unique_local = np.unique(rounded, axis=0, return_index=True)
    dup_mask = np.ones(len(inputs), dtype=bool)
    dup_mask[:] = False
    dup_mask[kept_indices] = True  # mark all kept
    unique_global = kept_indices[unique_local]
    dup_only = np.zeros(len(inputs), dtype=bool)
    dup_only[kept_indices] = True
    dup_only[unique_global] = False

    # 保护空闲样本: 如果去重后空闲样本不足，则恢复一些
    idle_after_dedup = idle_mask & keep & ~dup_only
    if idle_after_dedup.sum() < min_idle_keep:
        # 从被标记为重复的空闲样本中恢复
        idle_dup = idle_mask & dup_only
        restore_count = min(min_idle_keep - idle_after_dedup.sum(), idle_dup.sum())
        if restore_count > 0:
            restore_indices = np.where(idle_dup)[0][:restore_count]
            dup_only[restore_indices] = False
            print(f"  恢复空闲样本: {restore_count} (防止去重后空闲样本不足)")

    n_dedup = dup_only.sum()
    keep &= ~dup_only
    if n_dedup > 0:
        reasons['duplicates'] = n_dedup
        print(f"  移除重复样本: {n_dedup}")

    # 6. 移除正常数据中的极端离群点 (但保护空闲样本)
    if mode in ('moderate', 'aggressive'):
        normal_indices = np.where(normal_mask)[0]
        normal_z = analysis['normal_z']
        z_threshold = np.percentile(normal_z, 99) if mode == 'moderate' else np.percentile(normal_z, 97.5)

        outlier_mask = np.zeros(len(inputs), dtype=bool)
        for local_i, global_i in enumerate(normal_indices):
            if normal_z[local_i] > z_threshold:
                outlier_mask[global_i] = True

        # 不移除空闲样本
        outlier_mask &= ~idle_mask
        outlier_mask &= keep
        n_outlier = outlier_mask.sum()
        keep &= ~outlier_mask
        if n_outlier > 0:
            reasons['normal_outlier'] = n_outlier
            print(f"  移除正常离群点: {n_outlier} (已保护空闲样本)")

    # Summary
    clean_inputs = inputs[keep]
    clean_labels = labels[keep]
    removed = len(inputs) - len(clean_inputs)

    print(f"\n  清洗结果:")
    print(f"    原始: {len(inputs)} (正常={np.sum(labels==0)}, 异常={np.sum(labels==1)})")
    print(f"    清洗后: {len(clean_inputs)} (正常={np.sum(clean_labels==0)}, 异常={np.sum(clean_labels==1)})")
    print(f"    其中空闲样本: {(idle_mask & keep).sum()}")
    print(f"    移除: {removed} ({removed/len(inputs):.1%})")

    return clean_inputs, clean_labels, reasons


def main():
    parser = argparse.ArgumentParser(description='Security dataset analysis & cleaning')
    parser.add_argument('--data', type=str, default='../real_data/security_dataset.npz')
    parser.add_argument('--mode', type=str, default='moderate',
                        choices=['conservative', 'moderate', 'aggressive'],
                        help='清洗强度')
    parser.add_argument('--output', type=str, default=None,
                        help='输出路径 (默认: 同目录下 _cleaned.npz)')
    parser.add_argument('--dry-run', action='store_true',
                        help='只分析不保存')
    args = parser.parse_args()

    data = np.load(args.data)
    inputs = data['inputs'].astype(np.float32)
    labels = data['labels'].astype(np.int64)

    # 1. 数据分析
    analyze_data(inputs, labels)

    # 2. 标签一致性
    analysis = find_suspicious_labels(inputs, labels)

    # 3. 重复检查
    check_duplicates(inputs, labels)

    # 4. 清洗
    clean_inputs, clean_labels, reasons = clean_data(inputs, labels, analysis, mode=args.mode)

    if not args.dry_run:
        out_path = args.output or args.data.replace('.npz', '_cleaned.npz')
        np.savez(out_path, inputs=clean_inputs, labels=clean_labels)
        print(f"\n  已保存: {out_path}")

        # 同时保存被移除的样本 (方便人工审查)
        keep_mask = np.ones(len(inputs), dtype=bool)
        removed_mask = ~np.isin(np.arange(len(inputs)),
                                np.where(np.isin(inputs.view(np.void), clean_inputs.view(np.void)))[0])
        # 简化: 直接用索引
        print(f"\n建议下一步:")
        print(f"  python train.py --data {out_path}")
    else:
        print(f"\n  [dry-run] 未保存文件")


if __name__ == '__main__':
    main()
