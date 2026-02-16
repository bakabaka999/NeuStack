"""
training/security/export_onnx.py

将训练好的安全异常检测模型导出为 ONNX 格式

输出模型:
  - 输入: float32[batch, 8]  (ISecurityModel::Input)
  - 输出: float32[batch, 8]  (重构结果)
  - Metadata: anomaly_threshold

C++ 端 SecurityAnomalyModel 加载后:
  1. 前向传播得到重构输出
  2. 计算 MSE(input, output)
  3. MSE > threshold → 异常

用法:
    cd training/security
    python export_onnx.py
    python export_onnx.py --checkpoint checkpoints/best_model.pth
"""

import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from model import DeepAutoencoder, SimpleAutoencoder


def load_config(config_path: str):
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config):
    model_type = config['model']['type']

    if model_type == 'deep':
        return DeepAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model']['hidden_dims'],
            latent_dim=config['model']['latent_dim'],
            dropout=config['model'].get('dropout', 0.1),
        )
    elif model_type == 'simple':
        return SimpleAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model'].get('simple_hidden_dims', [32, 16]),
            latent_dim=config['model'].get('simple_latent_dim', 8),
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def export_onnx(model, input_dim, output_path, threshold=None):
    """导出 ONNX, 可选嵌入阈值 metadata"""
    model.eval()

    dummy = torch.randn(1, input_dim)

    torch.onnx.export(
        model,
        dummy,
        output_path,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={
            'input': {0: 'batch_size'},
            'output': {0: 'batch_size'},
        },
        opset_version=17,
        do_constant_folding=True,
    )
    print(f"Exported to {output_path}")

    # 嵌入 metadata
    onnx_model = onnx.load(output_path)

    if threshold is not None:
        onnx_model.metadata_props.append(
            onnx.StringStringEntryProto(key="anomaly_threshold", value=str(threshold))
        )
        print(f"Embedded threshold={threshold:.6f} into model metadata")

    # 标记模型类型
    onnx_model.metadata_props.append(
        onnx.StringStringEntryProto(key="model_type", value="security_anomaly")
    )
    onnx_model.metadata_props.append(
        onnx.StringStringEntryProto(key="input_features",
                                    value="pps_norm,bps_norm,syn_rate_norm,rst_rate_norm,"
                                          "syn_ratio_norm,new_conn_rate_norm,"
                                          "avg_pkt_size_norm,rst_ratio_norm")
    )

    onnx.save(onnx_model, output_path)
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")

    return output_path


def verify_onnx(pytorch_model, onnx_path, input_dim):
    """验证 PyTorch ↔ ONNX 输出一致性"""
    pytorch_model.eval()

    test_input = np.random.randn(5, input_dim).astype(np.float32)

    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'input': test_input})[0]

    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max diff (PyTorch vs ONNX): {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED! ✓")
        return True
    else:
        print("WARNING: Outputs differ significantly!")
        return False


def main():
    parser = argparse.ArgumentParser(description='Export security model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/best_model.pth')
    parser.add_argument('--threshold', type=str, default='checkpoints/threshold.txt',
                        help='Threshold file path or float value')
    args = parser.parse_args()

    config = load_config(args.config)

    # 加载模型
    model = create_model(config)
    model.load_state_dict(
        torch.load(args.checkpoint, map_location='cpu', weights_only=True)
    )
    model.eval()
    print(f"Loaded model from {args.checkpoint}")

    # 加载阈值
    threshold = None
    try:
        threshold = float(args.threshold)
    except ValueError:
        if os.path.exists(args.threshold):
            with open(args.threshold, 'r') as f:
                threshold = float(f.read().strip())
            print(f"Loaded threshold from {args.threshold}: {threshold:.6f}")
        else:
            print(f"Warning: threshold file not found: {args.threshold}")

    # 导出
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    export_onnx(model, config['model']['input_dim'], output_path, threshold)

    # 验证
    verify_onnx(model, output_path, config['model']['input_dim'])

    # 模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Summary")
    print("=" * 50)

    onnx_model = onnx.load(output_path)
    inp = onnx_model.graph.input[0]
    out = onnx_model.graph.output[0]
    print(f"Input:  {inp.name}  shape={[d.dim_value for d in inp.type.tensor_type.shape.dim]}")
    print(f"Output: {out.name}  shape={[d.dim_value for d in out.type.tensor_type.shape.dim]}")
    print(f"Size:   {os.path.getsize(output_path) / 1024:.2f} KB")

    if onnx_model.metadata_props:
        print("\nMetadata:")
        for prop in onnx_model.metadata_props:
            print(f"  {prop.key}: {prop.value}")


if __name__ == '__main__':
    main()
