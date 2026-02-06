import os
import yaml
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import deque

from env import SimpleNetworkEnv, MultiFlowEnv
from ddpg import DDPGAgent
from replay_buffer import ReplayBuffer


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def create_env(config: dict):
    env_type = config['env']['type']

    if env_type == 'simple':
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

    for _ in range(num_episodes):
        state = env.reset()
        episode_reward = 0
        throughputs = []
        rtts = []
        losses = []

        done = False
        while not done:
            action = agent.select_action(state, add_noise=False)
            next_state, reward, done, info = env.step(action[0])

            episode_reward += reward
            throughputs.append(info['throughput'])
            rtts.append(info['rtt'])
            losses.append(info['loss_rate'])

            state = next_state

        total_rewards.append(episode_reward)
        total_throughputs.append(np.mean(throughputs))
        total_rtts.append(np.mean(rtts))
        total_losses.append(np.mean(losses))

    return {
        'reward': np.mean(total_rewards),
        'throughput': np.mean(total_throughputs),
        'rtt': np.mean(total_rtts) * 1000,  # ms
        'loss_rate': np.mean(total_losses),
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
    # 移动平均
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
    )

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


def train_online(config: dict):
    """在线训练模式：与模拟环境交互"""
    # 创建环境和 agent
    env = create_env(config)
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
    )

    print(f"Environment: {config['env']['type']}")
    print(f"State dim: {env.state_dim}, Action dim: {env.action_dim}")
    print(f"Training for {config['training']['num_episodes']} episodes")

    # 训练
    episode_rewards = []
    eval_metrics = []
    noise_scale = 1.0

    for episode in range(config['training']['num_episodes']):
        state = env.reset()
        agent.noise.reset()
        episode_reward = 0

        for _ in range(config['training']['max_steps_per_episode']):
            # 选择动作
            action = agent.select_action(state, add_noise=True)
            action = action * noise_scale  # 噪声衰减
            action = np.clip(action, -1, 1)

            # 执行动作
            next_state, reward, done, _ = env.step(action[0])

            # 存储经验
            agent.store_transition(state, action, reward, next_state, done)

            # 更新网络
            if len(agent.replay_buffer) >= config['training']['warmup_steps']:
                agent.update()

            state = next_state
            episode_reward += reward

            if done:
                break

        episode_rewards.append(episode_reward)

        # 噪声衰减
        noise_scale = max(
            config['training']['min_noise'],
            noise_scale * config['training']['noise_decay']
        )

        # 打印进度
        if (episode + 1) % 10 == 0:
            avg_reward = np.mean(episode_rewards[-10:])
            print(f"Episode {episode + 1:4d} | Reward: {episode_reward:7.2f} | "
                  f"Avg(10): {avg_reward:7.2f} | Noise: {noise_scale:.3f}")

        # 评估
        if (episode + 1) % config['training']['eval_interval'] == 0:
            metrics = evaluate(agent, env)
            metrics['episode'] = episode + 1
            eval_metrics.append(metrics)
            print(f"  [Eval] Throughput: {metrics['throughput']/1e6:.2f} MB/s | "
                  f"RTT: {metrics['rtt']:.1f} ms | Loss: {metrics['loss_rate']*100:.2f}%")

        # 保存
        if (episode + 1) % config['training']['save_interval'] == 0:
            agent.save(os.path.join(config['output']['model_dir'], f'checkpoint_{episode+1}.pth'))

    # 保存最终模型
    agent.save(os.path.join(config['output']['model_dir'], 'final_model.pth'))

    # 绘制训练曲线
    plot_training(
        episode_rewards,
        eval_metrics,
        os.path.join(config['output']['model_dir'], 'training_curves.png')
    )

    print("\nTraining complete!")
    print(f"Final evaluation: {evaluate(agent, env)}")


if __name__ == '__main__':
    main()