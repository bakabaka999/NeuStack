import numpy as np
import os
from dataclasses import dataclass
from typing import Tuple

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False


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


# ============================================================================
# 真实数据统计 (从 4 组 tcp_samples 场景分析得到)
#
#   场景 1: local_normal   ~78 Mbps mean, RTT ~11ms
#   场景 2: local_tc       ~142 Mbps mean, RTT ~12ms, with tc shaping
#   场景 3: server_normal  ~19 Mbps mean, RTT ~22ms
#   场景 4: server_tc      ~18 Mbps mean, RTT ~157ms(!), with loss
#
# 综合: delivery_rate p5-p95 = [0.05, 35] MB/s
#       rtt_us p5-p95 = [281, 40815]
#       cwnd 大多数在 718 (max)
#       loss_rate 均值 0.06%, 仅 0.6% 样本非零
# ============================================================================

# 场景配置: (bandwidth_mbps, base_rtt_ms, buffer_packets, loss_base, label)
SCENARIO_PROFILES = [
    # 本机直连 (低延迟高带宽)
    {'bw_range': (40, 160), 'rtt_range': (2, 15), 'buf': 200, 'loss_base': 0.0001, 'label': 'local_normal'},
    # 本机 + tc (带宽被 shaping，波动大)
    {'bw_range': (10, 300), 'rtt_range': (1, 50), 'buf': 150, 'loss_base': 0.0001, 'label': 'local_tc'},
    # 远程服务器 (低带宽高延迟)
    {'bw_range': (5, 50), 'rtt_range': (5, 50), 'buf': 100, 'loss_base': 0.001, 'label': 'server_normal'},
    # 远程 + tc (极端条件)
    {'bw_range': (5, 50), 'rtt_range': (5, 200), 'buf': 80, 'loss_base': 0.003, 'label': 'server_tc'},
]


class CalibratedNetworkEnv:
    """
    用真实数据校准的网络模拟环境

    每个 episode 从 4 种真实场景中随机选取一个参数组合:
    - 带宽、RTT、缓冲区大小、基础丢包率都从真实数据分布中采样
    - 带宽变化使用多种模式 (平稳、阶跃、正弦、突发)
    - 奖励函数与 csv_to_dataset.py 中的 compute_orca_reward 完全对齐
    - 可选加载训练好的 bandwidth_predictor LSTM 提供 predicted_bw 特征
    """

    def __init__(
        self,
        mss: int = 1460,
        max_cwnd: int = 718,           # 真实数据中的最大 cwnd
        episode_steps: int = 300,
        step_interval_ms: float = 10,  # 每步模拟 10ms
        bw_predictor_path: str = None, # bandwidth_predictor.onnx 路径
        bw_history_len: int = 10,      # 带宽预测的历史窗口长度 (ONNX 模型期望 10)
    ):
        self.mss = mss
        self.max_cwnd = max_cwnd
        self.episode_steps = episode_steps
        self.step_interval = step_interval_ms / 1000  # seconds

        # 归一化参数 (与 csv_to_dataset.py 中 compute_orca_features 对齐)
        self.max_bw = 10e6  # MAX_BW = 10 MB/s (csv_to_dataset.py 中的归一化常量)

        # 带宽预测器
        self.bw_predictor = None
        self.bw_history_len = bw_history_len
        if bw_predictor_path and os.path.exists(bw_predictor_path):
            self._load_bw_predictor(bw_predictor_path)

        self.reset()

    def _load_bw_predictor(self, path: str):
        """加载训练好的 bandwidth_predictor"""
        if path.endswith('.onnx'):
            try:
                import onnxruntime as ort
                self.bw_predictor = ort.InferenceSession(path)
                self._bw_predictor_type = 'onnx'
                print(f"  Loaded bandwidth predictor (ONNX): {path}")
            except ImportError:
                print("  Warning: onnxruntime not installed, using analytical bandwidth estimation")
        elif path.endswith('.pth') and HAS_TORCH:
            # PyTorch checkpoint
            from model import LSTMBandwidthPredictor
            self.bw_predictor = LSTMBandwidthPredictor(input_dim=3, hidden_dim=64, num_layers=2)
            checkpoint = torch.load(path, map_location='cpu')
            if 'model_state_dict' in checkpoint:
                self.bw_predictor.load_state_dict(checkpoint['model_state_dict'])
            else:
                self.bw_predictor.load_state_dict(checkpoint)
            self.bw_predictor.eval()
            self._bw_predictor_type = 'torch'
            print(f"  Loaded bandwidth predictor (PyTorch): {path}")

    def reset(self) -> np.ndarray:
        """重置环境: 随机选取场景并初始化"""
        self.step_count = 0

        # 随机选取一个场景
        profile = SCENARIO_PROFILES[np.random.randint(len(SCENARIO_PROFILES))]
        self._current_profile = profile['label']

        # 从场景范围内随机采样参数
        bw_lo, bw_hi = profile['bw_range']
        rtt_lo, rtt_hi = profile['rtt_range']

        self.bandwidth = np.random.uniform(bw_lo, bw_hi) * 1e6 / 8  # bytes/s
        self.base_rtt = np.random.uniform(rtt_lo, rtt_hi) / 1000  # seconds
        self.buffer_size = profile['buf'] + np.random.randint(-20, 20)
        self.loss_base = profile['loss_base']

        # CUBIC 状态
        self.cwnd = 10  # slow start
        self.w_max = self.cwnd
        self.epoch_start = 0
        self.ssthresh = self.max_cwnd  # 初始无限大

        # 链路状态
        self.queue_size = 0.0
        self.bytes_in_flight = 0

        # 带宽变化模式
        self._init_bandwidth_pattern()

        # 历史记录 (用于带宽预测器输入)
        self._throughput_history = []
        self._rtt_history = []
        self._loss_history = []

        # 当前时刻的实际带宽估计 (滑动窗口最大 delivery_rate)
        self._delivery_rate_window = []
        self._est_bw = self.bandwidth  # 初始估计

        return self._get_obs()

    def _init_bandwidth_pattern(self):
        """初始化带宽变化模式 (从多种真实场景模式中随机选择)"""
        n = self.episode_steps
        t = np.linspace(0, 1, n)

        mode = np.random.choice(['stable', 'step', 'sine', 'burst', 'ramp'], p=[0.15, 0.25, 0.25, 0.2, 0.15])

        if mode == 'stable':
            # 平稳: 小幅随机波动
            pattern = self.bandwidth * (1 + 0.05 * np.random.randn(n))

        elif mode == 'step':
            # 阶跃: 模拟 tc 限速切换
            n_steps = np.random.randint(2, min(5, n // 10 + 2))
            pool = list(range(10, max(11, n - 10)))
            boundaries = sorted(np.random.choice(pool, min(n_steps - 1, len(pool)), replace=False))
            boundaries = [0] + list(boundaries) + [n]
            pattern = np.zeros(n)
            for i in range(len(boundaries) - 1):
                level = self.bandwidth * np.random.uniform(0.3, 1.5)
                pattern[boundaries[i]:boundaries[i+1]] = level
            # 加小幅噪声
            pattern += np.random.normal(0, self.bandwidth * 0.03, n)

        elif mode == 'sine':
            # 正弦: 周期性波动
            freq = np.random.uniform(1, 4)  # 1~4 个周期
            amplitude = np.random.uniform(0.2, 0.5)
            pattern = self.bandwidth * (1 + amplitude * np.sin(2 * np.pi * freq * t))
            pattern += np.random.normal(0, self.bandwidth * 0.05, n)

        elif mode == 'burst':
            # 突发: 正常水平 + 随机突发降低
            pattern = np.full(n, self.bandwidth, dtype=float)
            n_bursts = np.random.randint(2, 6)
            for _ in range(n_bursts):
                max_start = max(1, n - 30)
                start = np.random.randint(0, max_start)
                length = np.random.randint(5, min(40, n - start))
                drop = np.random.uniform(0.2, 0.6)
                end = min(start + length, n)
                pattern[start:end] *= drop
            pattern += np.random.normal(0, self.bandwidth * 0.03, n)

        elif mode == 'ramp':
            # 渐变: 带宽逐渐升高或降低
            start_mult = np.random.uniform(0.3, 0.8)
            end_mult = np.random.uniform(1.0, 1.5)
            if np.random.random() < 0.5:
                start_mult, end_mult = end_mult, start_mult
            pattern = self.bandwidth * np.linspace(start_mult, end_mult, n)
            pattern += np.random.normal(0, self.bandwidth * 0.03, n)

        # 保证正值
        self.bandwidth_pattern = np.clip(pattern, self.bandwidth * 0.1, self.bandwidth * 2.0)

    def _get_current_bandwidth(self) -> float:
        idx = min(self.step_count, len(self.bandwidth_pattern) - 1)
        return max(self.bandwidth_pattern[idx], 1.0)

    def _compute_cubic_cwnd(self) -> int:
        """计算 CUBIC cwnd (与真实协议栈一致)"""
        C = 0.4
        beta = 0.7
        t_sec = (self.step_count - self.epoch_start) * self.step_interval

        if self.w_max > 0:
            K = ((self.w_max * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t_sec - K) ** 3 + self.w_max
        else:
            w_cubic = self.cwnd + 1

        # Reno 部分: 线性增长
        w_reno = self.cwnd + 1.0 / max(self.cwnd, 1)

        # Slow start
        if self.cwnd < self.ssthresh:
            return min(self.cwnd * 2, self.ssthresh)

        return int(max(w_cubic, w_reno, 1))

    def step(self, alpha: float) -> Tuple[np.ndarray, float, bool, dict]:
        """
        执行一步

        alpha: RL 调制因子 ∈ [-1, 1]
        cwnd_new = cwnd_cubic * 2^alpha
        """
        self.step_count += 1

        current_bw = self._get_current_bandwidth()

        # CUBIC cwnd + alpha 调制
        cwnd_cubic = self._compute_cubic_cwnd()
        multiplier = 2 ** alpha
        new_cwnd = int(cwnd_cubic * multiplier)
        new_cwnd = int(np.clip(new_cwnd, 1, self.max_cwnd))

        # 发送速率
        send_rate = (new_cwnd * self.mss) / max(self.base_rtt, 0.0001)

        # 排队模型
        excess_rate = max(0, send_rate - current_bw)
        queue_growth = excess_rate * self.step_interval / self.mss
        self.queue_size += queue_growth

        # 排空
        drain = current_bw * self.step_interval / self.mss
        self.queue_size = max(0, self.queue_size - drain)
        self.queue_size = min(self.queue_size, self.buffer_size * 1.5)  # 允许小幅溢出

        # 丢包模型 (更细腻的阶梯式)
        q_ratio = self.queue_size / max(self.buffer_size, 1)
        if q_ratio >= 1.0:
            # 缓冲区满，尾丢弃
            loss_rate = 0.1 + 0.4 * min(q_ratio - 1.0, 0.5)
        elif q_ratio >= 0.8:
            loss_rate = 0.01 + 0.09 * (q_ratio - 0.8) / 0.2
        elif q_ratio >= 0.5:
            loss_rate = self.loss_base + 0.01 * (q_ratio - 0.5) / 0.3
        else:
            loss_rate = self.loss_base
        loss_rate = float(np.clip(loss_rate, 0, 0.5))

        # 随机丢包 (模拟真实网络中的随机性)
        if np.random.random() < 0.001:
            loss_rate = min(loss_rate + np.random.uniform(0.01, 0.05), 0.5)

        # 实际吞吐
        throughput = min(send_rate, current_bw) * (1 - loss_rate)

        # RTT = base + queuing_delay
        queuing_delay = (self.queue_size * self.mss) / max(current_bw, 1)
        rtt = self.base_rtt + queuing_delay
        # 真实 RTT 有抖动
        rtt *= (1 + np.random.uniform(-0.05, 0.05))
        rtt = max(rtt, self.base_rtt * 0.5)

        # CUBIC 丢包响应
        if loss_rate > 0.01:
            self.ssthresh = max(new_cwnd * 0.7, 2)
            self.w_max = new_cwnd
            self.epoch_start = self.step_count

        # 更新状态
        self.cwnd = new_cwnd
        self.bytes_in_flight = int(new_cwnd * self.mss * np.random.uniform(0.6, 0.95))

        # 更新历史 (用于带宽预测器)
        self._throughput_history.append(throughput)
        self._rtt_history.append(rtt)
        self._loss_history.append(loss_rate)
        self._delivery_rate_window.append(throughput)
        if len(self._delivery_rate_window) > 20:
            self._delivery_rate_window.pop(0)

        # 更新 est_bw (滑动窗口最大 delivery_rate，与 csv_to_dataset 一致)
        self._est_bw = max(self._delivery_rate_window) if self._delivery_rate_window else current_bw

        # 计算奖励 (用真实带宽计算利用率，而非 est_bw)
        reward = self._compute_reward(throughput, rtt, loss_rate, current_bw)

        done = self.step_count >= self.episode_steps

        info = {
            'throughput': throughput,
            'rtt': rtt,
            'loss_rate': loss_rate,
            'cwnd': self.cwnd,
            'cwnd_cubic': cwnd_cubic,
            'alpha': alpha,
            'bandwidth': current_bw,
            'queue_size': self.queue_size,
            'scenario': self._current_profile,
            'est_bw': self._est_bw,
        }

        return self._get_obs(), reward, done, info

    def _compute_reward(self, throughput: float, rtt: float, loss_rate: float, bandwidth: float) -> float:
        """
        奖励函数

        用真实链路带宽计算利用率 (环境知道真实带宽，agent 不知道)。
        这样 agent 不能通过降低发送量来欺骗 est_bw 获得虚高 reward。

        reward = 3*log(1 + throughput/bandwidth) - 0.3*sqrt(queuing_delay/base_rtt) - 5.0*loss_rate
        clipped to [-3, 2]
        """
        bw = max(bandwidth, 1)
        base_rtt = max(self.base_rtt, 0.0001)

        # 吞吐量奖励 (基于真实带宽的利用率)
        utilization = throughput / bw
        throughput_reward = 3.0 * np.log(1 + utilization)

        # 延迟惩罚 (减弱: 0.3 而非 0.5)
        queuing_delay = max(0, rtt - base_rtt)
        queuing_delay_ratio = min(queuing_delay / base_rtt, 5.0)
        delay_penalty = 0.3 * np.sqrt(queuing_delay_ratio)

        # 丢包惩罚 (加强: 5.0 而非 2.0，鼓励 agent 在高利用率下避免丢包)
        loss_penalty = 5.0 * loss_rate

        reward = throughput_reward - delay_penalty - loss_penalty
        return float(np.clip(reward, -3, 2))

    def _predict_bandwidth(self) -> float:
        """使用带宽预测器或退化为分析估计"""
        if self.bw_predictor is None or len(self._throughput_history) < self.bw_history_len:
            return self._est_bw

        # 构造输入: [1, seq_len, 3]
        # 归一化与 csv_to_dataset.py generate_bandwidth_dataset 一致:
        #   throughput_norm = delivery_rate / MAX_BW  (clip [0, 1])
        #   rtt_ratio = rtt / base_rtt                (clip [1, 5])
        #   loss_rate                                  (clip [0, 1])
        n = self.bw_history_len
        base_rtt = max(self.base_rtt, 0.0001)

        tp = np.clip(np.array(self._throughput_history[-n:]) / self.max_bw, 0, 1)
        rt = np.clip(np.array(self._rtt_history[-n:]) / base_rtt, 1, 5)
        lr = np.clip(np.array(self._loss_history[-n:]), 0, 1)
        seq = np.stack([tp, rt, lr], axis=-1).astype(np.float32)
        seq = seq.reshape(1, n, 3)

        if self._bw_predictor_type == 'onnx':
            input_name = self.bw_predictor.get_inputs()[0].name
            result = self.bw_predictor.run(None, {input_name: seq})
            pred_norm = float(result[0][0][0])
        elif self._bw_predictor_type == 'torch':
            with torch.no_grad():
                pred_norm = float(self.bw_predictor(torch.from_numpy(seq)).item())
        else:
            return self._est_bw

        # 反归一化: pred_norm ∈ [0, 1] -> bytes/s
        return max(pred_norm * self.max_bw, 1.0)

    def _get_obs(self) -> np.ndarray:
        """
        获取 7 维观测 (与 csv_to_dataset compute_orca_features 对齐 + est_bw)

        [0] throughput_norm      = delivery_rate / est_bw      (clip [0, 2])
        [1] queuing_delay_norm   = (rtt - min_rtt) / min_rtt   (clip [0, 5])
        [2] rtt_ratio            = rtt / min_rtt                (clip [1, 5])
        [3] loss_rate                                           (clip [0, 1])
        [4] cwnd_norm            = cwnd / bdp                   (clip [0, 10])
        [5] in_flight_ratio      = bytes_in_flight / (cwnd*MSS) (clip [0, 2])
        [6] est_bw_norm          = est_bw / MAX_BW              (带宽预测)
        """
        current_bw = self._get_current_bandwidth()
        base_rtt = max(self.base_rtt, 0.0001)
        est_bw = max(self._est_bw, 1)

        # 当前吞吐
        send_rate = (self.cwnd * self.mss) / base_rtt
        throughput = min(send_rate, current_bw)

        # RTT
        queuing_delay = (self.queue_size * self.mss) / max(current_bw, 1)
        rtt = base_rtt + queuing_delay

        # 丢包估计
        q_ratio = self.queue_size / max(self.buffer_size, 1)
        loss_rate = self.loss_base if q_ratio < 0.5 else min(0.05, q_ratio * 0.1)

        # BDP
        bdp = (est_bw * base_rtt) / self.mss
        bdp = max(bdp, 1)

        # 带宽预测
        predicted_bw = self._predict_bandwidth()

        obs = np.array([
            np.clip(throughput / est_bw, 0, 2),                              # throughput_norm
            np.clip(queuing_delay / base_rtt, 0, 5),                         # queuing_delay_norm
            np.clip(rtt / base_rtt, 1, 5),                                   # rtt_ratio
            np.clip(loss_rate, 0, 1),                                        # loss_rate
            np.clip(self.cwnd / bdp, 0, 10),                                 # cwnd_norm
            np.clip(self.bytes_in_flight / max(self.cwnd * self.mss, 1), 0, 2),  # in_flight_ratio
            np.clip(predicted_bw / self.max_bw, 0, 2),                      # est_bw_norm
        ], dtype=np.float32)

        return obs

    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1


class SimpleNetworkEnv:
    """
    简化的网络模拟环境 (保留用于对比测试)

    模拟一个瓶颈链路，带有：
    - 可变带宽
    - 排队延迟
    - 丢包
    """

    def __init__(
        self,
        bandwidth_mbps: float = 100.0,
        base_rtt_ms: float = 20.0,
        buffer_size_packets: int = 100,
        mss: int = 1460,
        max_cwnd: int = 1000,
        episode_steps: int = 200,
        bandwidth_variation: bool = True,
    ):
        self.bandwidth = bandwidth_mbps * 1e6 / 8
        self.base_rtt = base_rtt_ms / 1000
        self.buffer_size = buffer_size_packets
        self.mss = mss
        self.max_cwnd = max_cwnd
        self.episode_steps = episode_steps
        self.bandwidth_variation = bandwidth_variation

        self.max_throughput = 100e6
        self.max_delay = 0.1

        self.reset()

    def reset(self) -> np.ndarray:
        self.step_count = 0
        self.cwnd = 10
        self.bytes_in_flight = 0
        self.queue_size = 0
        self.w_max = self.cwnd
        self.epoch_start = 0
        self._init_bandwidth_pattern()
        return self._get_obs()

    def _init_bandwidth_pattern(self):
        if self.bandwidth_variation:
            t = np.linspace(0, 4 * np.pi, self.episode_steps)
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
        idx = min(self.step_count, len(self.bandwidth_pattern) - 1)
        return self.bandwidth_pattern[idx]

    def _compute_cubic_cwnd(self) -> int:
        C = 0.4
        beta = 0.7
        t = (self.step_count - self.epoch_start) * 0.01

        if self.w_max > 0:
            K = ((self.w_max * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t - K) ** 3 + self.w_max
        else:
            w_cubic = self.cwnd + 1

        w_reno = self.cwnd + 1
        return int(max(w_cubic, w_reno, 1))

    def step(self, alpha: float) -> Tuple[np.ndarray, float, bool, dict]:
        self.step_count += 1
        current_bw = self._get_current_bandwidth()

        cwnd_cubic = self._compute_cubic_cwnd()
        multiplier = 2 ** alpha
        new_cwnd = int(cwnd_cubic * multiplier)
        new_cwnd = np.clip(new_cwnd, 1, self.max_cwnd)

        send_rate = (new_cwnd * self.mss) / self.base_rtt
        excess_rate = max(0, send_rate - current_bw)
        queue_growth = excess_rate * 0.01
        self.queue_size = min(self.queue_size + queue_growth / self.mss, self.buffer_size)
        drain_rate = current_bw * 0.01 / self.mss
        self.queue_size = max(0, self.queue_size - drain_rate)

        if self.queue_size >= self.buffer_size * 0.9:
            loss_rate = 0.1 + 0.4 * (self.queue_size / self.buffer_size - 0.9) / 0.1
        elif self.queue_size >= self.buffer_size * 0.5:
            loss_rate = 0.01 + 0.09 * (self.queue_size / self.buffer_size - 0.5) / 0.4
        else:
            loss_rate = 0.001
        loss_rate = np.clip(loss_rate, 0, 0.5)

        throughput = min(send_rate, current_bw) * (1 - loss_rate)

        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay

        if loss_rate > 0.05:
            self.w_max = new_cwnd
            self.epoch_start = self.step_count

        self.cwnd = new_cwnd
        self.bytes_in_flight = int(new_cwnd * self.mss * 0.8)

        reward = self._compute_reward(throughput, rtt, loss_rate, current_bw)
        done = self.step_count >= self.episode_steps

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

    def _compute_reward(self, throughput, rtt, loss_rate, bandwidth):
        throughput_reward = throughput / bandwidth if bandwidth > 0 else 0
        queuing_delay = rtt - self.base_rtt
        delay_penalty = 2.0 * (queuing_delay / self.base_rtt)
        loss_penalty = 10.0 * loss_rate
        return throughput_reward - delay_penalty - loss_penalty

    def _get_obs(self) -> np.ndarray:
        current_bw = self._get_current_bandwidth()
        send_rate = (self.cwnd * self.mss) / self.base_rtt
        throughput = min(send_rate, current_bw)
        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay
        loss_rate = 0.001 if self.queue_size < self.buffer_size * 0.5 else 0.05
        predicted_bw = current_bw

        obs = np.array([
            throughput / self.max_throughput,
            queuing_delay / self.base_rtt,
            rtt / self.base_rtt,
            loss_rate,
            self.cwnd / self.max_cwnd,
            self.bytes_in_flight / (self.cwnd * self.mss + 1),
            predicted_bw / self.max_throughput,
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
    多流竞争环境 (保留用于对比测试)

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
        self.step_count = 0
        self.queue_size = 0
        for flow in self.flows:
            flow['cwnd'] = 10
            flow['bytes_in_flight'] = 0
            flow['w_max'] = 10
            flow['epoch_start'] = 0
        return self._get_obs(0)

    def step(self, alpha: float, flow_idx: int = 0) -> Tuple[np.ndarray, float, bool, dict]:
        self.step_count += 1

        total_send_rate = 0
        flow_send_rates = []

        for i, flow in enumerate(self.flows):
            if i == flow_idx:
                cwnd_cubic = self._compute_cubic_cwnd(flow)
                multiplier = 2 ** alpha
                flow['cwnd'] = int(np.clip(cwnd_cubic * multiplier, 1, self.max_cwnd))
            else:
                flow['cwnd'] = self._compute_cubic_cwnd(flow)
                flow['cwnd'] = int(np.clip(flow['cwnd'], 1, self.max_cwnd))

            send_rate = (flow['cwnd'] * self.mss) / self.base_rtt
            flow_send_rates.append(send_rate)
            total_send_rate += send_rate

        if total_send_rate > self.bandwidth:
            throughputs = [r / total_send_rate * self.bandwidth for r in flow_send_rates]
        else:
            throughputs = flow_send_rates

        excess = max(0, total_send_rate - self.bandwidth) * 0.01
        self.queue_size = min(self.queue_size + excess / self.mss, self.buffer_size)
        self.queue_size = max(0, self.queue_size - self.bandwidth * 0.01 / self.mss)

        if self.queue_size >= self.buffer_size * 0.8:
            loss_rate = 0.1
        else:
            loss_rate = 0.001

        if loss_rate > 0.05:
            for flow in self.flows:
                flow['w_max'] = flow['cwnd']
                flow['epoch_start'] = self.step_count

        queuing_delay = (self.queue_size * self.mss) / self.bandwidth
        rtt = self.base_rtt + queuing_delay

        my_throughput = throughputs[flow_idx] * (1 - loss_rate)

        reward = (
            my_throughput / self.bandwidth -
            2.0 * queuing_delay / self.base_rtt -
            10.0 * loss_rate
        )

        if len(throughputs) > 1:
            fairness = (sum(throughputs) ** 2) / (len(throughputs) * sum(t**2 for t in throughputs))
            reward += 0.5 * (fairness - 0.5)

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
            0.8,
            self.bandwidth / self.num_flows / self.max_throughput,
        ], dtype=np.float32)

    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1
