import numpy as np
from collections import deque
import random


class ReplayBuffer:
    """经验回放缓冲区"""

    def __init__(self, capacity: int = 100000):
        self.buffer = deque(maxlen=capacity)

    def push(self, state, action, reward, next_state, done):
        self.buffer.append((state, action, reward, next_state, done))

    def sample(self, batch_size: int):
        batch = random.sample(self.buffer, batch_size)

        states, actions, rewards, next_states, dones = zip(*batch)

        return (
            np.array(states, dtype=np.float32),
            np.array(actions, dtype=np.float32),
            np.array(rewards, dtype=np.float32).reshape(-1, 1),
            np.array(next_states, dtype=np.float32),
            np.array(dones, dtype=np.float32).reshape(-1, 1),
        )

    def load_from_npz(self, npz_path: str):
        """从 npz 文件加载离线数据"""
        data = np.load(npz_path)
        states = data['states']
        actions = data['actions']
        rewards = data['rewards']
        next_states = data['next_states']
        dones = data['dones']

        # 确保形状正确
        actions = actions.reshape(-1, 1) if actions.ndim == 1 else actions
        rewards = rewards.flatten()
        dones = dones.flatten()

        for i in range(len(states)):
            self.push(
                states[i],
                actions[i],
                rewards[i],
                next_states[i],
                dones[i]
            )

        return len(states)

    def __len__(self):
        return len(self.buffer)


class PrioritizedReplayBuffer:
    """优先经验回放（可选）"""

    def __init__(self, capacity: int = 100000, alpha: float = 0.6):
        self.capacity = capacity
        self.alpha = alpha
        self.buffer = []
        self.priorities = np.zeros(capacity, dtype=np.float32)
        self.pos = 0

    def push(self, state, action, reward, next_state, done):
        max_priority = self.priorities.max() if self.buffer else 1.0

        if len(self.buffer) < self.capacity:
            self.buffer.append((state, action, reward, next_state, done))
        else:
            self.buffer[self.pos] = (state, action, reward, next_state, done)

        self.priorities[self.pos] = max_priority
        self.pos = (self.pos + 1) % self.capacity

    def sample(self, batch_size: int, beta: float = 0.4):
        if len(self.buffer) == self.capacity:
            priorities = self.priorities
        else:
            priorities = self.priorities[:len(self.buffer)]

        probs = priorities ** self.alpha
        probs /= probs.sum()

        indices = np.random.choice(len(self.buffer), batch_size, p=probs)

        batch = [self.buffer[i] for i in indices]
        states, actions, rewards, next_states, dones = zip(*batch)

        # 重要性采样权重
        weights = (len(self.buffer) * probs[indices]) ** (-beta)
        weights /= weights.max()

        return (
            np.array(states, dtype=np.float32),
            np.array(actions, dtype=np.float32),
            np.array(rewards, dtype=np.float32).reshape(-1, 1),
            np.array(next_states, dtype=np.float32),
            np.array(dones, dtype=np.float32).reshape(-1, 1),
            indices,
            weights.astype(np.float32).reshape(-1, 1),
        )

    def update_priorities(self, indices, priorities):
        for idx, priority in zip(indices, priorities):
            self.priorities[idx] = priority + 1e-6

    def __len__(self):
        return len(self.buffer)