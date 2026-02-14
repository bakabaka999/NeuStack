"""
training/security/model.py

安全异常检测 Autoencoder 模型

为 ISecurityModel 提供训练端模型, 输入/输出均为 8 维.
C++ 端使用 MSE 重构误差判定异常.

两种架构:
  1. DeepAutoencoder   — 带 BatchNorm + GELU + Dropout, 真正的瓶颈 (latent=4)
  2. SimpleAutoencoder — 纯 MLP baseline (与 anomaly 模块一致)

选型理由 (为什么选 Deep AE 而非 VAE):
  - 8 维输入, VAE 的 KL 正则化收益有限, 但 ONNX 导出需处理采样层
  - BatchNorm 提供足够的正则化, 防止 identity mapping
  - latent=4 是真正的瓶颈 (8→4, 50% 压缩), 迫使模型学习有意义的表示
  - GELU 比 ReLU 梯度更平滑, 在小网络上收敛更好
  - 直接 input→output, MSE 计算与 C++ SecurityAnomalyModel 完全兼容
"""

import torch
import torch.nn as nn


class DeepAutoencoder(nn.Module):
    """
    深度 Autoencoder (BatchNorm + GELU + Dropout)

    结构:
      Encoder: input_dim → hidden[0] → hidden[1] → ... → latent_dim
      Decoder: latent_dim → hidden[-1] → hidden[-2] → ... → input_dim

    每个隐藏层: Linear → BatchNorm → GELU → Dropout
    输出层: Sigmoid (输出范围 [0, 1])
    """

    def __init__(
        self,
        input_dim: int = 8,
        hidden_dims: list = [64, 32],
        latent_dim: int = 4,
        dropout: float = 0.1,
    ):
        super().__init__()
        self.input_dim = input_dim

        # ─── Encoder ───
        encoder_layers = []
        prev_dim = input_dim
        for dim in hidden_dims:
            encoder_layers.extend([
                nn.Linear(prev_dim, dim),
                nn.BatchNorm1d(dim),
                nn.GELU(),
                nn.Dropout(dropout),
            ])
            prev_dim = dim
        encoder_layers.append(nn.Linear(prev_dim, latent_dim))
        self.encoder = nn.Sequential(*encoder_layers)

        # ─── Decoder (镜像结构) ───
        decoder_layers = []
        prev_dim = latent_dim
        for dim in reversed(hidden_dims):
            decoder_layers.extend([
                nn.Linear(prev_dim, dim),
                nn.BatchNorm1d(dim),
                nn.GELU(),
                nn.Dropout(dropout),
            ])
            prev_dim = dim
        decoder_layers.extend([
            nn.Linear(prev_dim, input_dim),
            nn.Sigmoid(),  # 输出 [0, 1]
        ])
        self.decoder = nn.Sequential(*decoder_layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        latent = self.encoder(x)
        reconstructed = self.decoder(latent)
        return reconstructed

    def encode(self, x: torch.Tensor) -> torch.Tensor:
        return self.encoder(x)

    def reconstruction_error(self, x: torch.Tensor) -> torch.Tensor:
        """逐样本 MSE 重构误差"""
        reconstructed = self.forward(x)
        return torch.mean((x - reconstructed) ** 2, dim=1)


class SimpleAutoencoder(nn.Module):
    """
    简化版 Autoencoder (纯 MLP, 无 BN/Dropout)

    作为 baseline 对照
    """

    def __init__(
        self,
        input_dim: int = 8,
        hidden_dims: list = [32, 16],
        latent_dim: int = 8,
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

        # Decoder
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
            nn.Sigmoid(),
        ])
        self.decoder = nn.Sequential(*decoder_layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        latent = self.encoder(x)
        reconstructed = self.decoder(latent)
        return reconstructed

    def reconstruction_error(self, x: torch.Tensor) -> torch.Tensor:
        reconstructed = self.forward(x)
        return torch.mean((x - reconstructed) ** 2, dim=1)
