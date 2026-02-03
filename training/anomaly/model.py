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
        """解码器：x -> latent"""
        # x: [batch, input_dim]
        x = self.relu(self.encoder_fc(x))           # [batch, hidden_dim]
        x = x.unsqueeze(1)                          # [batch, 1, hidden_dim]
        x, _ = self.encoder_lstm(x)                 # [batch, 1, hidden_dim]
        x = x.squeeze(1)                            # [batch, hidden_dim]
        latent = self.encoder_latent(x)             # [batch, latent_dim]
        return latent
    
    def decode(self, latent: torch.Tensor) -> torch.Tensor:
        """解码器: latent -> reconstructed"""
        x = self.relu(self.decoder_latent(latent))   # [batch, hidden_dim]
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