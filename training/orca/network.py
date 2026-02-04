import torch
import torch.nn as nn
import torch.nn.functional as F


class Actor(nn.Module):
    """
    Actor 网络：状态 → 动作 (α)

    输出 tanh 激活，确保 α ∈ [-1, 1]
    """

    def __init__(
        self,
        state_dim: int = 7,
        hidden_dims: list = [256, 256],
        action_dim: int = 1
    ):
        super().__init__()

        layers = []
        prev_dim = state_dim

        for hidden_dim in hidden_dims:
            layers.append(nn.Linear(prev_dim, hidden_dim))
            layers.append(nn.ReLU())
            prev_dim = hidden_dim

        self.net = nn.Sequential(*layers)
        self.out = nn.Linear(prev_dim, action_dim)

    def forward(self, state: torch.Tensor) -> torch.Tensor:
        """
        Args:
            state: [batch, state_dim]
        Returns:
            action: [batch, action_dim], 范围 [-1, 1]
        """
        x = self.net(state)
        action = torch.tanh(self.out(x))
        return action


class Critic(nn.Module):
    """
    Critic 网络：(状态, 动作) → Q 值
    """

    def __init__(
        self,
        state_dim: int = 7,
        action_dim: int = 1,
        hidden_dims: list = [256, 256]
    ):
        super().__init__()

        layers = []
        prev_dim = state_dim + action_dim

        for hidden_dim in hidden_dims:
            layers.append(nn.Linear(prev_dim, hidden_dim))
            layers.append(nn.ReLU())
            prev_dim = hidden_dim

        layers.append(nn.Linear(prev_dim, 1))

        self.net = nn.Sequential(*layers)

    def forward(self, state: torch.Tensor, action: torch.Tensor) -> torch.Tensor:
        """
        Args:
            state: [batch, state_dim]
            action: [batch, action_dim]
        Returns:
            q_value: [batch, 1]
        """
        x = torch.cat([state, action], dim=-1)
        return self.net(x)


class OrcaActor(nn.Module):
    """
    轻量版 Actor（用于部署）

    更小的网络，适合实时推理
    """

    def __init__(self, state_dim: int = 7, hidden_dim: int = 64):
        super().__init__()

        self.net = nn.Sequential(
            nn.Linear(state_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
            nn.Tanh()
        )

    def forward(self, state: torch.Tensor) -> torch.Tensor:
        return self.net(state)