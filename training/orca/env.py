import numpy as np
from dataclasses import dataclass
from typing import Tuple, Optional

@dataclass
class NetworkState:
    """网络状态"""
    throughput: float      # bytes/s
    rtt: float             # seconds
    min_rtt: float         # seconds
    loss_rate: float       # [0, 1]
    cwnd: int              # packets
    bytes_in_flight: int   # bytes
    predicted_bw: float    # bytes/s (来自带宽预测)


class SimpleNetworkEnv:
    """
    简化的网络模拟环境

    模拟一个瓶颈链路，带有：
    - 可变带宽
    - 排队延迟
    - 丢包
    """

    def __init__(
        self,
        bandwidth_mbps: float = 100.0,      # 瓶颈带宽 Mbps
        base_rtt_ms: float = 20.0,          # 基础 RTT ms
        buffer_size_packets: int = 100,     # 瓶颈缓冲区大小
        mss: int = 1460,                    # MSS
        max_cwnd: int = 1000,               # 最大 cwnd
        episode_steps: int = 200,           # 每 episode 步数
        bandwidth_variation: bool = True,   # 是否模拟带宽波动
    ):
        self.bandwidth = bandwidth_mbps * 1e6 / 8  # bytes/s
        self.base_rtt = base_rtt_ms / 1000 # second
        self.buffer_size = buffer_size_packets
        self.mss = mss
        self.max_cwnd = max_cwnd
        self.episode_steps = episode_steps
        self.bandwidth_variation = bandwidth_variation

        # 归一化参数
        self.max_throughput = 100e6  # 100 MB/s
        self.max_delay = 0.1         # 100ms

        self.reset()

    def reset(self) -> np.ndarray:
        """重置环境，返回初始状态"""
        self.step_count = 0
        self.cwnd = 10  # 初始 cwnd
        self.bytes_in_flight = 0
        self.queue_size = 0

        # CUBIC 状态
        self.w_max = self.cwnd
        self.epoch_start = 0

        # 带宽变化
        self._init_bandwidth_pattern()

        return self._get_obs()
    
    def _init_bandwidth_pattern(self):
        """初始化带宽变化模式"""
        if self.bandwidth_variation:
            # 生成一个随机的带宽变化序列
            t = np.linspace(0, 4 * np.pi, self.episode_steps)
            # 基础带宽 + 正弦波动 + 随机突变
            self.bandwidth_pattern = (
                self.bandwidth * (0.7 + 0.3 * np.sin(t)) +
                np.random.normal(0, self.bandwidth * 0.1, self.episode_steps)
            )
            self.bandwidth_pattern = np.clip(
                self.bandwidth_pattern,
                self.bandwidth * 0.3,
                self.bandwidth * 1.2
            )
        else:
            self.bandwidth_pattern = np.full(self.episode_steps, self.bandwidth)

    def _get_current_bandwidth(self) -> float:
        """获取当前时刻的带宽"""
        idx = min(self.step_count, len(self.bandwidth_pattern) - 1)
        return self.bandwidth_pattern[idx]

    def _compute_cubic_cwnd(self) -> int:
        """计算 CUBIC 的 cwnd 建议值"""
        C = 0.4
        beta = 0.7
        t = (self.step_count - self.epoch_start) * 0.01  # 假设 10ms 一步

        if self.w_max > 0:
            K = ((self.w_max * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t - K) ** 3 + self.w_max
        else:
            w_cubic = self.cwnd + 1

        # Reno 线性增长
        w_reno = self.cwnd + 1

        return int(max(w_cubic, w_reno, 1))
    
    def step(self, alpha: float) -> Tuple[np.ndarray, float, bool, dict]:
        """
        执行一步

        Args:
            alpha: RL 输出的调制因子 ∈ [-1, 1]

        Returns:
            obs: 新状态
            reward: 奖励
            done: 是否结束
            info: 额外信息
        """
        self.step_count += 1

        # 获取当前宽带
        current_bw = self._get_current_bandwidth()

        # 计算 CUBIC cwnd 并应用 alpha 调制
        cwnd_cubic = self._compute_cubic_cwnd()
        multiplier = 2 ** alpha  # ∈ [0.5, 2.0]
        new_cwnd = int(cwnd_cubic * multiplier)
        new_cwnd = np.clip(new_cwnd, 1, self.max_cwnd)

        # 计算发送速率与排队
        send_rate = (new_cwnd * self.mss) / self.base_rtt  # bytes/s

        # 排队 = 发送速率超过带宽的部分
        excess_rate = max(0, send_rate - current_bw)
        queue_growth = excess_rate * 0.01  # 10ms 的累积
        self.queue_size = min(self.queue_size + queue_growth / self.mss, self.buffer_size)

        # 队列排空
        drain_rate = current_bw * 0.01 / self.mss
        self.queue_size = max(0, self.queue_size - drain_rate)

        # 计算丢包率
        if self.queue_size >= self.buffer_size * 0.9:
            loss_rate = 0.1 + 0.4 * (self.queue_size / self.buffer_size - 0.9) / 0.1
        elif self.queue_size >= self.buffer_size * 0.5:
            loss_rate = 0.01 + 0.09 * (self.queue_size / self.buffer_size - 0.5) / 0.4
        else:
            loss_rate = 0.001
        loss_rate = np.clip(loss_rate, 0, 0.5)

        # 计算实际吞吐量
        throughput = min(send_rate, current_bw) * (1 - loss_rate)

        # 计算 RTT
        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay

        # 处理丢包事件（更新 CUBIC 状态）
        if loss_rate > 0.05:
            self.w_max = new_cwnd
            self.epoch_start = self.step_count

        # 更新状态
        self.cwnd = new_cwnd
        self.bytes_in_flight = int(new_cwnd * self.mss * 0.8)  # 假设 80% 在飞

        # 计算奖励
        reward = self._compute_reward(throughput, rtt, loss_rate, current_bw)

        # 检查是否结束
        done = self.step_count >= self.episode_steps

        # 额外信息
        info = {
            'throughput': throughput,
            'rtt': rtt,
            'loss_rate': loss_rate,
            'cwnd': self.cwnd,
            'cwnd_cubic': cwnd_cubic,
            'alpha': alpha,
            'bandwidth': current_bw,
            'queue_size': self.queue_size,
        }

        return self._get_obs(), reward, done, info

    def _compute_reward(
        self,
        throughput: float,
        rtt: float,
        loss_rate: float,
        bandwidth: float
    ) -> float:
        """计算奖励"""
        # 吞吐量奖励 (归一化到 [0, 1])
        throughput_reward = throughput / bandwidth if bandwidth > 0 else 0

        # 延迟惩罚
        queuing_delay = rtt - self.base_rtt
        delay_penalty = 2.0 * (queuing_delay / self.base_rtt)

        # 丢包惩罚
        loss_penalty = 10.0 * loss_rate

        reward = throughput_reward - delay_penalty - loss_penalty

        return reward
    
    def _get_obs(self) -> np.ndarray:
        """获取当前观测（7 维状态）"""
        current_bw = self._get_current_bandwidth()

        # 估算当前状态
        send_rate = (self.cwnd * self.mss) / self.base_rtt
        throughput = min(send_rate, current_bw)
        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay

        # 简单估计丢包率
        loss_rate = 0.001 if self.queue_size < self.buffer_size * 0.5 else 0.05

        # 带宽预测 (这里简化为当前带宽，实际应该用模型预测)
        predicted_bw = current_bw

        obs = np.array([
            throughput / self.max_throughput,                    # throughput_normalized
            queuing_delay / self.base_rtt,                       # queuing_delay_normalized
            rtt / self.base_rtt,                                 # rtt_ratio
            loss_rate,                                           # loss_rate
            self.cwnd / self.max_cwnd,                           # cwnd_normalized
            self.bytes_in_flight / (self.cwnd * self.mss + 1),   # in_flight_ratio
            predicted_bw / self.max_throughput,                  # predicted_bw_normalized
        ], dtype=np.float32)

        return obs
    
    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1


class MultiFlowEnv:
    """
    多流竞争环境

    模拟多个 TCP 流竞争同一瓶颈带宽
    """

    def __init__(
        self,
        num_flows: int = 2,
        bandwidth_mbps: float = 100.0,
        base_rtt_ms: float = 20.0,
        **kwargs
    ):
        self.num_flows = num_flows
        self.bandwidth = bandwidth_mbps * 1e6 / 8

        # 为每个流创建独立状态
        self.flows = [
            {
                'cwnd': 10,
                'bytes_in_flight': 0,
                'w_max': 10,
                'epoch_start': 0,
            }
            for _ in range(num_flows)
        ]

        self.base_rtt = base_rtt_ms / 1000
        self.buffer_size = kwargs.get('buffer_size_packets', 100)
        self.mss = kwargs.get('mss', 1460)
        self.max_cwnd = kwargs.get('max_cwnd', 1000)
        self.max_throughput = 100e6

        self.step_count = 0
        self.episode_steps = kwargs.get('episode_steps', 200)
        self.queue_size = 0

    def reset(self) -> np.ndarray:
        """重置，返回第一个流的状态"""
        self.step_count = 0
        self.queue_size = 0

        for flow in self.flows:
            flow['cwnd'] = 10
            flow['bytes_in_flight'] = 0
            flow['w_max'] = 10
            flow['epoch_start'] = 0

        return self._get_obs(0)

    def step(self, alpha: float, flow_idx: int = 0) -> Tuple[np.ndarray, float, bool, dict]:
        """
        执行一步（只控制指定流，其他流用 CUBIC）
        """
        self.step_count += 1

        # 所有流的发送速率总和
        total_send_rate = 0
        flow_send_rates = []

        for i, flow in enumerate(self.flows):
            if i == flow_idx:
                # 被 RL 控制的流
                cwnd_cubic = self._compute_cubic_cwnd(flow)
                multiplier = 2 ** alpha
                flow['cwnd'] = int(np.clip(cwnd_cubic * multiplier, 1, self.max_cwnd))
            else:
                # 其他流用纯 CUBIC
                flow['cwnd'] = self._compute_cubic_cwnd(flow)
                flow['cwnd'] = int(np.clip(flow['cwnd'], 1, self.max_cwnd))

            send_rate = (flow['cwnd'] * self.mss) / self.base_rtt
            flow_send_rates.append(send_rate)
            total_send_rate += send_rate

        # 带宽公平分配
        if total_send_rate > self.bandwidth:
            # 按比例分配
            throughputs = [
                r / total_send_rate * self.bandwidth
                for r in flow_send_rates
            ]
        else:
            throughputs = flow_send_rates

        # 更新队列
        excess = max(0, total_send_rate - self.bandwidth) * 0.01
        self.queue_size = min(self.queue_size + excess / self.mss, self.buffer_size)
        self.queue_size = max(0, self.queue_size - self.bandwidth * 0.01 / self.mss)

        # 计算丢包率（所有流共享）
        if self.queue_size >= self.buffer_size * 0.8:
            loss_rate = 0.1
        else:
            loss_rate = 0.001

        # 更新 CUBIC 状态
        if loss_rate > 0.05:
            for flow in self.flows:
                flow['w_max'] = flow['cwnd']
                flow['epoch_start'] = self.step_count

        # 计算 RTT
        queuing_delay = (self.queue_size * self.mss) / self.bandwidth
        rtt = self.base_rtt + queuing_delay

        # 被控制流的奖励
        my_throughput = throughputs[flow_idx] * (1 - loss_rate)

        # 奖励：吞吐 - 延迟惩罚 - 丢包惩罚 + 公平性奖励
        reward = (
            my_throughput / self.bandwidth -
            2.0 * queuing_delay / self.base_rtt -
            10.0 * loss_rate
        )

        # 公平性奖励（Jain's fairness index）
        if len(throughputs) > 1:
            fairness = (sum(throughputs) ** 2) / (len(throughputs) * sum(t**2 for t in throughputs))
            reward += 0.5 * (fairness - 0.5)  # 鼓励公平

        done = self.step_count >= self.episode_steps

        info = {
            'throughput': my_throughput,
            'rtt': rtt,
            'loss_rate': loss_rate,
            'cwnd': self.flows[flow_idx]['cwnd'],
            'all_throughputs': throughputs,
        }

        return self._get_obs(flow_idx), reward, done, info

    def _compute_cubic_cwnd(self, flow: dict) -> int:
        C = 0.4
        beta = 0.7
        t = (self.step_count - flow['epoch_start']) * 0.01

        if flow['w_max'] > 0:
            K = ((flow['w_max'] * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t - K) ** 3 + flow['w_max']
        else:
            w_cubic = flow['cwnd'] + 1

        return int(max(w_cubic, flow['cwnd'] + 1, 1))

    def _get_obs(self, flow_idx: int) -> np.ndarray:
        flow = self.flows[flow_idx]

        send_rate = (flow['cwnd'] * self.mss) / self.base_rtt
        throughput = min(send_rate, self.bandwidth / self.num_flows)
        queuing_delay = (self.queue_size * self.mss) / self.bandwidth
        rtt = self.base_rtt + queuing_delay
        loss_rate = 0.001 if self.queue_size < self.buffer_size * 0.5 else 0.05

        return np.array([
            throughput / self.max_throughput,
            queuing_delay / self.base_rtt,
            rtt / self.base_rtt,
            loss_rate,
            flow['cwnd'] / self.max_cwnd,
            0.8,  # in_flight_ratio
            self.bandwidth / self.num_flows / self.max_throughput,  # predicted_bw
        ], dtype=np.float32)

    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1
