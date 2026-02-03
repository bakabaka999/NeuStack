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
        input_dim: int = 3,           # 输入维度
        hidden_dim: int = 64,         # 隐藏层维度
        num_layers: int = 2,          # 层数
        dropout: float = 0.2,
        bidirectional: bool = False   # 是否双向
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