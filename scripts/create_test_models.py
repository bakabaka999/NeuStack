#!/usr/bin/env python3
"""
生成测试用的 ONNX 模型

用法:
    pip install torch onnx
    python scripts/create_test_models.py

生成的模型:
    models/test_simple.onnx     - 最简单的测试模型 (3 -> 1)
    models/orca_actor.onnx      - Orca 拥塞控制 (6 -> 1)
    models/anomaly_detector.onnx - 异常检测 Autoencoder (5 -> 5)
    models/bandwidth_predictor.onnx - 带宽预测 (30 -> 1)
"""

import os
import torch
import torch.nn as nn

# 确保 models 目录存在
os.makedirs("models", exist_ok=True)


# ============================================================================
# 1. 最简单的测试模型
# ============================================================================
class SimpleModel(nn.Module):
    """最简单的测试模型: 3 输入 -> 1 输出"""
    def __init__(self):
        super().__init__()
        self.fc = nn.Linear(3, 1)

    def forward(self, x):
        return self.fc(x)


# ============================================================================
# 2. Orca Actor 模型 (拥塞控制)
# ============================================================================
class OrcaActor(nn.Module):
    """
    Orca 拥塞控制 Actor 网络

    输入 (6维):
        - throughput_normalized
        - queuing_delay_normalized
        - rtt_ratio
        - loss_rate
        - cwnd_normalized
        - in_flight_ratio

    输出 (1维):
        - alpha ∈ [-1, 1], cwnd_new = 2^alpha * cwnd_base
    """
    def __init__(self, input_dim=6, hidden_dim=64):
        super().__init__()
        self.fc1 = nn.Linear(input_dim, hidden_dim)
        self.fc2 = nn.Linear(hidden_dim, hidden_dim)
        self.fc3 = nn.Linear(hidden_dim, 1)

    def forward(self, x):
        x = torch.relu(self.fc1(x))
        x = torch.relu(self.fc2(x))
        x = torch.tanh(self.fc3(x))  # 输出 [-1, 1]
        return x


# ============================================================================
# 3. 异常检测 Autoencoder
# ============================================================================
class AnomalyAutoencoder(nn.Module):
    """
    LSTM-Autoencoder 异常检测 (简化为 MLP 版本用于测试)

    输入 (5维):
        - syn_rate
        - rst_rate
        - new_conn_rate
        - packet_rate
        - avg_packet_size

    输出 (5维): 重构输入，用于计算重构误差
    """
    def __init__(self, input_dim=5, hidden_dim=16):
        super().__init__()
        # Encoder
        self.encoder = nn.Sequential(
            nn.Linear(input_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.ReLU(),
        )
        # Decoder
        self.decoder = nn.Sequential(
            nn.Linear(hidden_dim // 2, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, input_dim),
        )

    def forward(self, x):
        encoded = self.encoder(x)
        decoded = self.decoder(encoded)
        return decoded


# ============================================================================
# 4. 带宽预测模型
# ============================================================================
class BandwidthPredictor(nn.Module):
    """
    带宽预测模型 (简化为 MLP 版本用于测试)

    输入 (30维): 10 个时间步 × 3 特征 (throughput, rtt, loss)
    输出 (1维): 预测带宽 (归一化)
    """
    def __init__(self, history_len=10, features_per_step=3, hidden_dim=32):
        super().__init__()
        input_dim = history_len * features_per_step  # 30
        self.fc1 = nn.Linear(input_dim, hidden_dim)
        self.fc2 = nn.Linear(hidden_dim, hidden_dim)
        self.fc3 = nn.Linear(hidden_dim, 1)

    def forward(self, x):
        x = torch.relu(self.fc1(x))
        x = torch.relu(self.fc2(x))
        x = torch.sigmoid(self.fc3(x))  # 输出 [0, 1]
        return x


# ============================================================================
# 导出函数
# ============================================================================
def export_model(model, input_shape, output_path, input_name="input", output_name="output"):
    """导出模型为 ONNX 格式"""
    model.eval()

    # 创建示例输入
    dummy_input = torch.randn(1, *input_shape)

    # 导出
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=[input_name],
        output_names=[output_name],
        dynamic_axes={
            input_name: {0: "batch_size"},
            output_name: {0: "batch_size"}
        },
        opset_version=17,
        do_constant_folding=True,
    )

    print(f"✓ Exported: {output_path}")
    print(f"  Input:  {input_name} shape={list(dummy_input.shape)}")

    # 验证
    with torch.no_grad():
        output = model(dummy_input)
    print(f"  Output: {output_name} shape={list(output.shape)}")


def main():
    print("=" * 60)
    print("NeuStack Test Model Generator")
    print("=" * 60)
    print()

    # 1. 简单测试模型
    print("[1/4] Creating simple test model...")
    model = SimpleModel()
    export_model(model, (3,), "models/test_simple.onnx")
    print()

    # 2. Orca Actor
    print("[2/4] Creating Orca Actor model...")
    model = OrcaActor()
    export_model(model, (6,), "models/orca_actor.onnx")
    print()

    # 3. Anomaly Detector
    print("[3/4] Creating Anomaly Detector model...")
    model = AnomalyAutoencoder()
    export_model(model, (5,), "models/anomaly_detector.onnx")
    print()

    # 4. Bandwidth Predictor
    print("[4/4] Creating Bandwidth Predictor model...")
    model = BandwidthPredictor()
    export_model(model, (30,), "models/bandwidth_predictor.onnx")
    print()

    print("=" * 60)
    print("All models created successfully!")
    print()
    print("Models saved to:")
    print("  models/test_simple.onnx        (3 -> 1)")
    print("  models/orca_actor.onnx         (6 -> 1)")
    print("  models/anomaly_detector.onnx   (5 -> 5)")
    print("  models/bandwidth_predictor.onnx (30 -> 1)")
    print()
    print("Note: These are randomly initialized models for testing.")
    print("      Real models need to be trained with actual data.")
    print("=" * 60)


if __name__ == "__main__":
    main()
