import torch
import torch.nn as nn
import torch.optim as optim
import torch.nn.functional as F
import numpy as np
from typing import Tuple

from replay_buffer import ReplayBuffer


LOG_STD_MIN = -5
LOG_STD_MAX = 2


class SACActor(nn.Module):
    """
    SAC 随机策略网络: state → (mean, log_std) → tanh(Normal(mean, std))

    输出 tanh 压缩的高斯分布，确保 action ∈ [-1, 1]
    """

    def __init__(self, state_dim: int, hidden_dims: list, action_dim: int):
        super().__init__()

        layers = []
        prev_dim = state_dim
        for h in hidden_dims:
            layers.append(nn.Linear(prev_dim, h))
            layers.append(nn.ReLU())
            prev_dim = h

        self.net = nn.Sequential(*layers)
        self.mean_head = nn.Linear(prev_dim, action_dim)
        self.log_std_head = nn.Linear(prev_dim, action_dim)

    def forward(self, state: torch.Tensor):
        x = self.net(state)
        mean = self.mean_head(x)
        log_std = self.log_std_head(x)
        log_std = torch.clamp(log_std, LOG_STD_MIN, LOG_STD_MAX)
        return mean, log_std

    def sample(self, state: torch.Tensor):
        """采样动作并计算 log_prob (用 reparameterization trick)"""
        mean, log_std = self.forward(state)
        std = log_std.exp()
        normal = torch.distributions.Normal(mean, std)

        # Reparameterization: z = mean + std * eps
        z = normal.rsample()
        action = torch.tanh(z)

        # Log prob with tanh correction
        log_prob = normal.log_prob(z) - torch.log(1 - action.pow(2) + 1e-6)
        log_prob = log_prob.sum(dim=-1, keepdim=True)

        return action, log_prob, mean

    def get_deterministic_action(self, state: torch.Tensor) -> torch.Tensor:
        """确定性动作 (eval 用): 直接取 tanh(mean)"""
        mean, _ = self.forward(state)
        return torch.tanh(mean)


class SACCritic(nn.Module):
    """
    SAC Twin Critic: 两个独立的 Q 网络

    取 min(Q1, Q2) 防止 Q 值高估
    """

    def __init__(self, state_dim: int, action_dim: int, hidden_dims: list):
        super().__init__()

        # Q1
        layers1 = []
        prev_dim = state_dim + action_dim
        for h in hidden_dims:
            layers1.append(nn.Linear(prev_dim, h))
            layers1.append(nn.ReLU())
            prev_dim = h
        layers1.append(nn.Linear(prev_dim, 1))
        self.q1 = nn.Sequential(*layers1)

        # Q2
        layers2 = []
        prev_dim = state_dim + action_dim
        for h in hidden_dims:
            layers2.append(nn.Linear(prev_dim, h))
            layers2.append(nn.ReLU())
            prev_dim = h
        layers2.append(nn.Linear(prev_dim, 1))
        self.q2 = nn.Sequential(*layers2)

    def forward(self, state: torch.Tensor, action: torch.Tensor):
        x = torch.cat([state, action], dim=-1)
        return self.q1(x), self.q2(x)

    def q1_forward(self, state: torch.Tensor, action: torch.Tensor):
        x = torch.cat([state, action], dim=-1)
        return self.q1(x)


class SACAgent:
    """
    Soft Actor-Critic

    相比 DDPG 的优势:
    - Twin Critic 防止 Q 值高估 (解决 Critic 发散)
    - 自动 entropy 调节 (不需要手动 noise schedule)
    - 随机策略 (eval 取均值，天然稳定)
    """

    def __init__(
        self,
        state_dim: int = 7,
        action_dim: int = 1,
        hidden_dims: list = [256, 256],
        lr_actor: float = 3e-4,
        lr_critic: float = 3e-4,
        lr_alpha: float = 3e-4,
        gamma: float = 0.99,
        tau: float = 0.005,
        buffer_size: int = 200000,
        batch_size: int = 256,
        init_alpha: float = 0.2,
        device: str = 'cpu',
    ):
        self.device = torch.device(device)
        self.gamma = gamma
        self.tau = tau
        self.batch_size = batch_size
        self.action_dim = action_dim

        # Actor (随机策略)
        self.actor = SACActor(state_dim, hidden_dims, action_dim).to(self.device)
        self.actor_optimizer = optim.Adam(self.actor.parameters(), lr=lr_actor)

        # Twin Critic
        self.critic = SACCritic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target = SACCritic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target.load_state_dict(self.critic.state_dict())
        self.critic_optimizer = optim.Adam(self.critic.parameters(), lr=lr_critic)

        # 自动 entropy 调节
        self.target_entropy = -action_dim  # 常用启发式: -dim(A)
        self.log_alpha = torch.tensor(np.log(init_alpha), dtype=torch.float32, requires_grad=True, device=self.device)
        self.alpha_optimizer = optim.Adam([self.log_alpha], lr=lr_alpha)

        # 经验回放
        self.replay_buffer = ReplayBuffer(buffer_size)

        # 兼容接口: DDPG 的 noise 接口 (SAC 不需要，但 train.py 会调用)
        self.noise = _DummyNoise()

    @property
    def alpha(self):
        return self.log_alpha.exp()

    def select_action(self, state: np.ndarray, add_noise: bool = True) -> np.ndarray:
        """选择动作"""
        state_t = torch.FloatTensor(state).unsqueeze(0).to(self.device)

        with torch.no_grad():
            if add_noise:
                action, _, _ = self.actor.sample(state_t)
            else:
                action = self.actor.get_deterministic_action(state_t)

        return action.cpu().numpy()[0]

    def store_transition(self, state, action, reward, next_state, done):
        self.replay_buffer.push(state, action, reward, next_state, done)

    def update(self, bc_weight: float = 0.0) -> Tuple[float, float]:
        """
        更新网络

        返回 (critic_loss, actor_loss) 以兼容 train.py 接口
        """
        if len(self.replay_buffer) < self.batch_size:
            return 0.0, 0.0

        states, actions, rewards, next_states, dones = self.replay_buffer.sample(self.batch_size)

        states = torch.FloatTensor(states).to(self.device)
        actions = torch.FloatTensor(actions).to(self.device)
        rewards = torch.FloatTensor(rewards).to(self.device)
        next_states = torch.FloatTensor(next_states).to(self.device)
        dones = torch.FloatTensor(dones).to(self.device)

        # ─── 更新 Critic ───
        with torch.no_grad():
            next_actions, next_log_probs, _ = self.actor.sample(next_states)
            q1_next, q2_next = self.critic_target(next_states, next_actions)
            q_next = torch.min(q1_next, q2_next) - self.alpha * next_log_probs
            target_q = rewards + (1 - dones) * self.gamma * q_next
            target_q = torch.clamp(target_q, -60, 40)

        q1, q2 = self.critic(states, actions)
        critic_loss = F.mse_loss(q1, target_q) + F.mse_loss(q2, target_q)

        self.critic_optimizer.zero_grad()
        critic_loss.backward()
        nn.utils.clip_grad_norm_(self.critic.parameters(), max_norm=1.0)
        self.critic_optimizer.step()

        # ─── 更新 Actor ───
        new_actions, log_probs, _ = self.actor.sample(states)
        q1_new, q2_new = self.critic(states, new_actions)
        q_new = torch.min(q1_new, q2_new)

        actor_loss = (self.alpha.detach() * log_probs - q_new).mean()

        if bc_weight > 0:
            bc_loss = F.mse_loss(new_actions, actions)
            actor_loss = actor_loss + bc_weight * bc_loss

        self.actor_optimizer.zero_grad()
        actor_loss.backward()
        nn.utils.clip_grad_norm_(self.actor.parameters(), max_norm=1.0)
        self.actor_optimizer.step()

        # ─── 更新 alpha (entropy temperature) ───
        alpha_loss = -(self.log_alpha * (log_probs + self.target_entropy).detach()).mean()

        self.alpha_optimizer.zero_grad()
        alpha_loss.backward()
        self.alpha_optimizer.step()

        # ─── 软更新 target critic ───
        for p, tp in zip(self.critic.parameters(), self.critic_target.parameters()):
            tp.data.copy_(self.tau * p.data + (1 - self.tau) * tp.data)

        return critic_loss.item(), actor_loss.item()

    def save(self, path: str):
        torch.save({
            'actor': self.actor.state_dict(),
            'critic': self.critic.state_dict(),
            'critic_target': self.critic_target.state_dict(),
            'log_alpha': self.log_alpha.detach().cpu(),
            'actor_optimizer': self.actor_optimizer.state_dict(),
            'critic_optimizer': self.critic_optimizer.state_dict(),
            'alpha_optimizer': self.alpha_optimizer.state_dict(),
        }, path)

    def load(self, path: str):
        ckpt = torch.load(path, map_location=self.device)
        self.actor.load_state_dict(ckpt['actor'])
        self.critic.load_state_dict(ckpt['critic'])
        self.critic_target.load_state_dict(ckpt['critic_target'])
        self.log_alpha = ckpt['log_alpha'].to(self.device).requires_grad_(True)
        self.alpha_optimizer = optim.Adam([self.log_alpha], lr=self.alpha_optimizer.defaults['lr'])
        self.actor_optimizer.load_state_dict(ckpt['actor_optimizer'])
        self.critic_optimizer.load_state_dict(ckpt['critic_optimizer'])
        self.alpha_optimizer.load_state_dict(ckpt['alpha_optimizer'])


class _DummyNoise:
    """兼容 DDPG 的 noise 接口"""
    def reset(self):
        pass
    def sample(self):
        return 0
