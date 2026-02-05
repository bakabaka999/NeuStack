import torch
import torch.nn as nn
import torch.optim as optim
import numpy as np
from typing import Tuple

from network import Actor, Critic, OrcaActor
from replay_buffer import ReplayBuffer


class OUNoise:
    """Ornstein-Uhlenbeck 噪声（用于探索）"""

    def __init__(
        self,
        action_dim: int,
        mu: float = 0.0,
        theta: float = 0.15,
        sigma: float = 0.2
    ):
        self.action_dim = action_dim
        self.mu = mu
        self.theta = theta
        self.sigma = sigma
        self.state = np.ones(action_dim) * mu

    def reset(self):
        self.state = np.ones(self.action_dim) * self.mu

    def sample(self) -> np.ndarray:
        dx = self.theta * (self.mu - self.state) + self.sigma * np.random.randn(self.action_dim)
        self.state += dx
        return self.state


class DDPGAgent:
    """DDPG 智能体"""

    def __init__(
        self,
        state_dim: int = 7,
        action_dim: int = 1,
        hidden_dims: list = [256, 256],
        lr_actor: float = 1e-4,
        lr_critic: float = 1e-3,
        gamma: float = 0.99,
        tau: float = 0.005,
        buffer_size: int = 100000,
        batch_size: int = 256,
        device: str = 'cpu'
    ):
        self.device = torch.device(device)
        self.gamma = gamma
        self.tau = tau
        self.batch_size = batch_size

        # 创建网络
        self.actor = Actor(state_dim, hidden_dims, action_dim).to(self.device)
        self.actor_target = Actor(state_dim, hidden_dims, action_dim).to(self.device)
        self.actor_target.load_state_dict(self.actor.state_dict())

        self.critic = Critic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target = Critic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target.load_state_dict(self.critic.state_dict())

        # 优化器
        self.actor_optimizer = optim.Adam(self.actor.parameters(), lr=lr_actor)
        self.critic_optimizer = optim.Adam(self.critic.parameters(), lr=lr_critic)

        # 经验回放
        self.replay_buffer = ReplayBuffer(buffer_size)

        # 探索噪声
        self.noise = OUNoise(action_dim)

    def select_action(self, state: np.ndarray, add_noise: bool = True) -> np.ndarray:
        """选择动作"""
        state_tensor = torch.FloatTensor(state).unsqueeze(0).to(self.device)

        self.actor.eval()
        with torch.no_grad():
            action = self.actor(state_tensor).cpu().numpy()[0]
        self.actor.train()

        if add_noise:
            action += self.noise.sample()
            action = np.clip(action, -1, 1)

        return action

    def store_transition(self, state, action, reward, next_state, done):
        """存储经验"""
        self.replay_buffer.push(state, action, reward, next_state, done)

    def update(self, bc_weight: float = 0.0) -> Tuple[float, float]:
        """
        更新网络

        参数:
            bc_weight: 行为克隆正则化权重 (离线训练时使用)
                       0.0 = 纯 DDPG (在线训练)
                       >0  = TD3+BC 风格 (离线训练，推荐 2.5)
        """
        if len(self.replay_buffer) < self.batch_size:
            return 0.0, 0.0

        # 采样
        states, actions, rewards, next_states, dones = self.replay_buffer.sample(self.batch_size)

        states = torch.FloatTensor(states).to(self.device)
        actions = torch.FloatTensor(actions).to(self.device)
        rewards = torch.FloatTensor(rewards).to(self.device)
        next_states = torch.FloatTensor(next_states).to(self.device)
        dones = torch.FloatTensor(dones).to(self.device)

        # ─── 更新 Critic ───
        with torch.no_grad():
            next_actions = self.actor_target(next_states)
            target_q = self.critic_target(next_states, next_actions)
            target_q = rewards + (1 - dones) * self.gamma * target_q
            # 限制 target Q 范围，防止自举误差导致发散
            # reward 在 [-2, 1]，γ=0.95 时理论 Q ∈ [-40, 20]
            target_q = torch.clamp(target_q, -50, 25)

        current_q = self.critic(states, actions)
        critic_loss = nn.MSELoss()(current_q, target_q)

        self.critic_optimizer.zero_grad()
        critic_loss.backward()
        # 梯度裁剪防止爆炸
        nn.utils.clip_grad_norm_(self.critic.parameters(), max_norm=1.0)
        self.critic_optimizer.step()

        # ─── 更新 Actor ───
        # TD3+BC: actor_loss = λ * (-Q(s, π(s))) + ||π(s) - a||²
        predicted_actions = self.actor(states)
        q_values = self.critic(states, predicted_actions)

        if bc_weight > 0:
            # 行为克隆正则化：让 Actor 保持接近数据中的动作
            bc_loss = nn.MSELoss()(predicted_actions, actions)
            # 归一化 λ 使 Q-loss 和 BC-loss 在相同尺度 (TD3+BC 论文的技巧)
            lmbda = bc_weight / (q_values.abs().mean().detach() + 1e-8)
            actor_loss = -lmbda * q_values.mean() + bc_loss
        else:
            actor_loss = -q_values.mean()

        self.actor_optimizer.zero_grad()
        actor_loss.backward()
        # 梯度裁剪防止爆炸
        nn.utils.clip_grad_norm_(self.actor.parameters(), max_norm=1.0)
        self.actor_optimizer.step()

        # ─── 软更新 target 网络 ───
        self._soft_update(self.actor, self.actor_target)
        self._soft_update(self.critic, self.critic_target)

        return critic_loss.item(), actor_loss.item()

    def _soft_update(self, source: nn.Module, target: nn.Module):
        """软更新 target 网络"""
        for src_param, tgt_param in zip(source.parameters(), target.parameters()):
            tgt_param.data.copy_(self.tau * src_param.data + (1 - self.tau) * tgt_param.data)

    def save(self, path: str):
        """保存模型"""
        torch.save({
            'actor': self.actor.state_dict(),
            'actor_target': self.actor_target.state_dict(),
            'critic': self.critic.state_dict(),
            'critic_target': self.critic_target.state_dict(),
            'actor_optimizer': self.actor_optimizer.state_dict(),
            'critic_optimizer': self.critic_optimizer.state_dict(),
        }, path)

    def load(self, path: str):
        """加载模型"""
        checkpoint = torch.load(path, map_location=self.device)
        self.actor.load_state_dict(checkpoint['actor'])
        self.actor_target.load_state_dict(checkpoint['actor_target'])
        self.critic.load_state_dict(checkpoint['critic'])
        self.critic_target.load_state_dict(checkpoint['critic_target'])
        self.actor_optimizer.load_state_dict(checkpoint['actor_optimizer'])
        self.critic_optimizer.load_state_dict(checkpoint['critic_optimizer'])
