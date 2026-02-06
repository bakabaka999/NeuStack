import os
import yaml
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import deque

import torch

from env import SimpleNetworkEnv, MultiFlowEnv, CalibratedNetworkEnv
from ddpg import DDPGAgent
from sac import SACAgent
from replay_buffer import ReplayBuffer


def get_device(config: dict) -> str:
    """自动检测设备: 优先使用配置，否则自动检测 CUDA"""
    device = config.get('agent', {}).get('device', 'auto')
    if device == 'auto':
        if torch.cuda.is_available():
            device = 'cuda'
        elif hasattr(torch.backends, 'mps') and torch.backends.mps.is_available():
            device = 'mps'
        else:
            device = 'cpu'
    return device


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def create_env(config: dict):
    env_type = config['env']['type']

    if env_type == 'calibrated':
        bw_pred_path = config['env'].get('bw_predictor_path', None)
        return CalibratedNetworkEnv(
            max_cwnd=config['env'].get('max_cwnd', 718),
            episode_steps=config['env']['episode_steps'],
            step_interval_ms=config['env'].get('step_interval_ms', 10),
            bw_predictor_path=bw_pred_path,
            bw_history_len=config['env'].get('bw_history_len', 30),
        )
    elif env_type == 'simple':
        return SimpleNetworkEnv(
            bandwidth_mbps=config['env']['bandwidth_mbps'],
            base_rtt_ms=config['env']['base_rtt_ms'],
            buffer_size_packets=config['env']['buffer_size_packets'],
            episode_steps=config['env']['episode_steps'],
            bandwidth_variation=config['env']['bandwidth_variation'],
        )
    elif env_type == 'multi_flow':
        return MultiFlowEnv(
            num_flows=config['env']['num_flows'],
            bandwidth_mbps=config['env']['bandwidth_mbps'],
            base_rtt_ms=config['env']['base_rtt_ms'],
            buffer_size_packets=config['env']['buffer_size_packets'],
            episode_steps=config['env']['episode_steps'],
        )
    else:
        raise ValueError(f"Unknown env type: {env_type}")


def evaluate(agent: DDPGAgent, env, num_episodes: int = 5) -> dict:
    """评估 agent"""
    total_rewards = []
    total_throughputs = []
    total_rtts = []
    total_losses = []
    total_utilizations = []  # throughput / bandwidth 利用率

    for _ in range(num_episodes):
        state = env.reset()
        episode_reward = 0
        throughputs = []
        rtts = []
        losses = []
        utilizations = []
        steps = 0

        done = False
        while not done:
            action = agent.select_action(state, add_noise=False)
            next_state, reward, done, info = env.step(action[0])

            episode_reward += reward
            throughputs.append(info['throughput'])
            rtts.append(info['rtt'])
            losses.append(info['loss_rate'])
            if 'bandwidth' in info and info['bandwidth'] > 0:
                utilizations.append(info['throughput'] / info['bandwidth'])
            steps += 1

            state = next_state

        total_rewards.append(episode_reward / max(steps, 1))  # 平均每步 reward
        total_throughputs.append(np.mean(throughputs))
        total_rtts.append(np.mean(rtts))
        total_losses.append(np.mean(losses))
        if utilizations:
            total_utilizations.append(np.mean(utilizations))

    return {
        'reward': np.mean(total_rewards),           # 平均每步 reward
        'throughput': np.mean(total_throughputs),
        'rtt': np.mean(total_rtts) * 1000,          # ms
        'loss_rate': np.mean(total_losses),
        'utilization': np.mean(total_utilizations) if total_utilizations else 0,
    }


def plot_training(
    rewards: list,
    eval_metrics: list,
    save_path: str
):
    """绘制训练曲线"""
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))

    # Episode Rewards
    ax = axes[0, 0]
    ax.plot(rewards, alpha=0.3)
    window = min(50, len(rewards) // 10 + 1)
    if len(rewards) >= window:
        ma = np.convolve(rewards, np.ones(window)/window, mode='valid')
        ax.plot(range(window-1, len(rewards)), ma, 'r-', linewidth=2)
    ax.set_xlabel('Episode')
    ax.set_ylabel('Reward')
    ax.set_title('Training Rewards')
    ax.grid(True)

    if eval_metrics:
        episodes = [m['episode'] for m in eval_metrics]

        # Throughput
        ax = axes[0, 1]
        ax.plot(episodes, [m['throughput'] / 1e6 for m in eval_metrics], 'b-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('Throughput (MB/s)')
        ax.set_title('Evaluation Throughput')
        ax.grid(True)

        # RTT
        ax = axes[1, 0]
        ax.plot(episodes, [m['rtt'] for m in eval_metrics], 'g-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('RTT (ms)')
        ax.set_title('Evaluation RTT')
        ax.grid(True)

        # Loss Rate
        ax = axes[1, 1]
        ax.plot(episodes, [m['loss_rate'] * 100 for m in eval_metrics], 'r-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('Loss Rate (%)')
        ax.set_title('Evaluation Loss Rate')
        ax.grid(True)

    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()


def plot_offline_training(
    critic_losses: list,
    actor_losses: list,
    save_path: str
):
    """绘制离线训练曲线"""
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    # Critic Loss
    ax = axes[0]
    ax.plot(critic_losses, alpha=0.5)
    window = min(100, len(critic_losses) // 10 + 1)
    if len(critic_losses) >= window:
        ma = np.convolve(critic_losses, np.ones(window)/window, mode='valid')
        ax.plot(range(window-1, len(critic_losses)), ma, 'r-', linewidth=2)
    ax.set_xlabel('Update Step')
    ax.set_ylabel('Critic Loss')
    ax.set_title('Critic Loss')
    ax.grid(True)

    # Actor Loss
    ax = axes[1]
    ax.plot(actor_losses, alpha=0.5)
    if len(actor_losses) >= window:
        ma = np.convolve(actor_losses, np.ones(window)/window, mode='valid')
        ax.plot(range(window-1, len(actor_losses)), ma, 'r-', linewidth=2)
    ax.set_xlabel('Update Step')
    ax.set_ylabel('Actor Loss')
    ax.set_title('Actor Loss')
    ax.grid(True)

    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()


def train_offline(config: dict, data_path: str, output_dir: str):
    """
    离线训练模式：从 npz 文件加载数据训练

    使用 TD3+BC 算法防止 Q 值发散：
    - bc_weight > 0: 添加行为克隆正则化，防止 Actor 选择 OOD 动作
    """
    print("=" * 50)
    print("  Offline Training Mode (TD3+BC)")
    print("=" * 50)

    device = get_device(config)

    # 创建 agent
    agent = DDPGAgent(
        state_dim=config['agent']['state_dim'],
        action_dim=config['agent']['action_dim'],
        hidden_dims=config['agent']['hidden_dims'],
        lr_actor=config['agent']['lr_actor'],
        lr_critic=config['agent']['lr_critic'],
        gamma=config['agent']['gamma'],
        tau=config['agent']['tau'],
        buffer_size=config['agent']['buffer_size'],
        batch_size=config['agent']['batch_size'],
        device=device,
    )
    print(f"Device: {agent.device}")

    # 加载数据到 replay buffer
    print(f"Loading data from: {data_path}")
    n_samples = agent.replay_buffer.load_from_npz(data_path)
    print(f"Loaded {n_samples} transitions")

    # 训练参数
    num_updates = config.get('offline_training', {}).get('num_updates', 10000)
    log_interval = config.get('offline_training', {}).get('log_interval', 100)
    save_interval = config.get('offline_training', {}).get('save_interval', 1000)
    bc_weight = config.get('offline_training', {}).get('bc_weight', 1.0)

    print(f"Training for {num_updates} updates")
    print(f"Batch size: {config['agent']['batch_size']}")
    print(f"BC weight: {bc_weight}")
    print()

    # 训练循环
    critic_losses = []
    actor_losses = []

    for step in range(num_updates):
        critic_loss, actor_loss = agent.update(bc_weight=bc_weight)
        critic_losses.append(critic_loss)
        actor_losses.append(actor_loss)

        if (step + 1) % log_interval == 0:
            avg_critic = np.mean(critic_losses[-log_interval:])
            avg_actor = np.mean(actor_losses[-log_interval:])
            print(f"Step {step + 1:5d}/{num_updates} | "
                  f"Critic Loss: {avg_critic:.6f} | Actor Loss: {avg_actor:.6f}")

        if (step + 1) % save_interval == 0:
            agent.save(os.path.join(output_dir, f'checkpoint_{step+1}.pth'))

    # 保存最终模型
    agent.save(os.path.join(output_dir, 'final_model.pth'))

    # 绘制训练曲线
    plot_offline_training(
        critic_losses,
        actor_losses,
        os.path.join(output_dir, 'offline_training_curves.png')
    )

    print()
    print("=" * 50)
    print("  Offline Training Complete!")
    print("=" * 50)
    print(f"Final Critic Loss: {np.mean(critic_losses[-100:]):.6f}")
    print(f"Final Actor Loss: {np.mean(actor_losses[-100:]):.6f}")
    print(f"Model saved to: {output_dir}/final_model.pth")


def create_agent(config: dict):
    """根据配置创建 agent (SAC 或 DDPG)"""
    algo = config['agent'].get('algorithm', 'sac')
    device = get_device(config)

    if algo == 'sac':
        return SACAgent(
            state_dim=config['agent']['state_dim'],
            action_dim=config['agent']['action_dim'],
            hidden_dims=config['agent']['hidden_dims'],
            lr_actor=config['agent']['lr_actor'],
            lr_critic=config['agent']['lr_critic'],
            gamma=config['agent']['gamma'],
            tau=config['agent']['tau'],
            buffer_size=config['agent']['buffer_size'],
            batch_size=config['agent']['batch_size'],
            device=device,
        )
    else:
        return DDPGAgent(
            state_dim=config['agent']['state_dim'],
            action_dim=config['agent']['action_dim'],
            hidden_dims=config['agent']['hidden_dims'],
            lr_actor=config['agent']['lr_actor'],
            lr_critic=config['agent']['lr_critic'],
            gamma=config['agent']['gamma'],
            tau=config['agent']['tau'],
            buffer_size=config['agent']['buffer_size'],
            batch_size=config['agent']['batch_size'],
            device=device,
        )


def train_online(config: dict):
    """
    在线训练模式：与模拟环境交互

    支持从离线数据预热 replay buffer，让训练从数据经验开始，
    然后通过在线交互逐步矫正 Q 值。
    """
    # 创建环境和 agent
    env = create_env(config)
    agent = create_agent(config)

    print("=" * 50)
    print("  Online Training Mode (Calibrated Env)")
    print("=" * 50)
    algo = config['agent'].get('algorithm', 'sac')
    print(f"Algorithm: {algo.upper()}")
    print(f"Device: {agent.device}")
    print(f"Environment: {config['env']['type']}")
    print(f"State dim: {env.state_dim}, Action dim: {env.action_dim}")
    print(f"Training for {config['training']['num_episodes']} episodes")

    # 离线数据预热 (可选)
    warmup_data = config.get('training', {}).get('warmup_data', None)
    if warmup_data and os.path.exists(warmup_data):
        n = agent.replay_buffer.load_from_npz(warmup_data)
        print(f"Loaded {n} offline transitions for warmup")

        # 用离线数据预训练几轮 (纯 BC 模式，不用 Q)
        pretrain_steps = config.get('training', {}).get('pretrain_steps', 1000)
        if pretrain_steps > 0 and n >= agent.batch_size:
            print(f"Pre-training actor with BC for {pretrain_steps} steps...")
            for step in range(pretrain_steps):
                agent.update(bc_weight=2.0)
                if (step + 1) % 200 == 0:
                    print(f"  Pretrain step {step + 1}/{pretrain_steps}")
            print("  Pre-training done!")
    print()

    # 训练
    episode_rewards = []
    eval_metrics = []
    best_reward = -float('inf')
    is_sac = isinstance(agent, SACAgent)

    for episode in range(config['training']['num_episodes']):
        state = env.reset()
        agent.noise.reset()
        episode_reward = 0

        for _ in range(config['training']['max_steps_per_episode']):
            # 选择动作 (SAC 自带探索，DDPG 靠外部噪声)
            action = agent.select_action(state, add_noise=True)
            action = np.clip(action, -1, 1)

            # 执行动作
            next_state, reward, done, _ = env.step(action[0])

            # 存储经验
            agent.store_transition(state, action, reward, next_state, done)

            # 更新网络
            if len(agent.replay_buffer) >= config['training']['warmup_steps']:
                agent.update(bc_weight=0.0)

            state = next_state
            episode_reward += reward

            if done:
                break

        episode_rewards.append(episode_reward)

        # 打印进度
        if (episode + 1) % 10 == 0:
            avg_reward = np.mean(episode_rewards[-10:])
            extra = ""
            if is_sac:
                extra = f" | Alpha: {agent.alpha.item():.3f}"
            print(f"Episode {episode + 1:4d} | Reward: {episode_reward:7.2f} | "
                  f"Avg(10): {avg_reward:7.2f}{extra}")

        # 评估
        if (episode + 1) % config['training']['eval_interval'] == 0:
            metrics = evaluate(agent, env)
            metrics['episode'] = episode + 1
            eval_metrics.append(metrics)
            print(f"  [Eval] Reward/step: {metrics['reward']:.3f} | "
                  f"Utilization: {metrics['utilization']*100:.1f}% | "
                  f"RTT: {metrics['rtt']:.1f} ms | Loss: {metrics['loss_rate']*100:.3f}%")

            # 保存最优模型
            if metrics['reward'] > best_reward:
                best_reward = metrics['reward']
                agent.save(os.path.join(config['output']['model_dir'], 'best_model.pth'))
                print(f"  [Best] New best reward: {best_reward:.2f}")

        # 定期保存
        if (episode + 1) % config['training']['save_interval'] == 0:
            agent.save(os.path.join(config['output']['model_dir'], f'checkpoint_{episode+1}.pth'))

    # 保存最终模型
    agent.save(os.path.join(config['output']['model_dir'], 'final_model.pth'))

    # 导出 ONNX
    onnx_path = config['output'].get('onnx_path', None)
    if onnx_path:
        export_onnx(agent, config['agent']['state_dim'], onnx_path)

    # 绘制训练曲线
    plot_training(
        episode_rewards,
        eval_metrics,
        os.path.join(config['output']['model_dir'], 'training_curves.png')
    )

    print()
    print("=" * 50)
    print("  Online Training Complete!")
    print("=" * 50)
    final_eval = evaluate(agent, env, num_episodes=10)
    print(f"Final evaluation (10 episodes):")
    print(f"  Reward/step:  {final_eval['reward']:.3f}")
    print(f"  Utilization:  {final_eval['utilization']*100:.1f}%")
    print(f"  Throughput:   {final_eval['throughput']/1e6:.2f} MB/s")
    print(f"  RTT:          {final_eval['rtt']:.1f} ms")
    print(f"  Loss Rate:    {final_eval['loss_rate']*100:.3f}%")
    print(f"Best reward during training: {best_reward:.2f}")


def export_onnx(agent, state_dim: int, onnx_path: str):
    """导出 Actor 为 ONNX (兼容 SAC 和 DDPG)"""
    import torch

    # SAC actor 输出 (mean, log_std)，需要包装成只输出 tanh(mean)
    if isinstance(agent, SACAgent):
        class DeterministicWrapper(torch.nn.Module):
            def __init__(self, actor):
                super().__init__()
                self.actor = actor
            def forward(self, state):
                return self.actor.get_deterministic_action(state)
        export_model = DeterministicWrapper(agent.actor).cpu()
    else:
        export_model = agent.actor.cpu()

    export_model.eval()
    dummy_input = torch.randn(1, state_dim)

    os.makedirs(os.path.dirname(onnx_path) or '.', exist_ok=True)
    torch.onnx.export(
        export_model,
        dummy_input,
        onnx_path,
        input_names=['state'],
        output_names=['alpha'],
        dynamic_axes={'state': {0: 'batch'}, 'alpha': {0: 'batch'}},
        opset_version=11,
    )
    print(f"Exported ONNX model to: {onnx_path}")


def main():
    parser = argparse.ArgumentParser(description='Train Orca DDPG')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--data', type=str, default=None,
                        help='Path to npz data file (enables offline training)')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 创建输出目录
    os.makedirs(config['output']['model_dir'], exist_ok=True)

    # 根据是否提供数据文件选择训练模式
    if args.data:
        # 离线训练模式
        train_offline(config, args.data, config['output']['model_dir'])
    else:
        # 在线训练模式
        train_online(config)


if __name__ == '__main__':
    main()
