import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from model import SimpleAutoencoder, LSTMAutoencoder


def load_config(config_path: str):
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def create_model(config):
    model_type = config['model']['type']

    if model_type == 'simple':
        return SimpleAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dims=config['model']['hidden_dims'],
            latent_dim=config['model']['latent_dim']
        )
    elif model_type == 'lstm':
        return LSTMAutoencoder(
            input_dim=config['model']['input_dim'],
            hidden_dim=config['model'].get('hidden_dim', 32),
            latent_dim=config['model']['latent_dim'],
            num_layers=config['model'].get('num_layers', 1)
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


def export_onnx(model, input_dim, output_path, threshold: float = None):
    """导出模型到 ONNX 格式"""
    model.eval()

    # 创建示例输入
    dummy_input = torch.randn(1, input_dim)

    # 导出
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={
            'input': {0: 'batch_size'},
            'output': {0: 'batch_size'}
        },
        opset_version=17,
        do_constant_folding=True
    )

    print(f"Exported to {output_path}")

    # 加载并添加 metadata
    onnx_model = onnx.load(output_path)

    # 嵌入阈值到 metadata
    if threshold is not None:
        onnx_model.metadata_props.append(
            onnx.StringStringEntryProto(key="anomaly_threshold", value=str(threshold))
        )
        print(f"Embedded threshold={threshold:.6f} into model metadata")

    # 保存
    onnx.save(onnx_model, output_path)

    # 验证导出的模型
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")

    return output_path


def verify_onnx(pytorch_model, onnx_path, input_dim):
    """验证 ONNX 模型与 PyTorch 模型输出一致"""
    pytorch_model.eval()

    # 创建测试输入
    test_input = np.random.randn(5, input_dim).astype(np.float32)

    # PyTorch 推理
    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    # ONNX Runtime 推理
    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'input': test_input})[0]

    # 比较输出
    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max difference between PyTorch and ONNX: {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED!")
        return True
    else:
        print("WARNING: Outputs differ significantly!")
        return False


def main():
    parser = argparse.ArgumentParser(description='Export anomaly model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/best_model.pth')
    parser.add_argument('--threshold', type=str, default='checkpoints/threshold.txt',
                        help='Path to threshold file (or a float value)')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 创建并加载模型
    model = create_model(config)
    model.load_state_dict(torch.load(args.checkpoint, map_location='cpu', weights_only=True))
    model.eval()
    print(f"Loaded model from {args.checkpoint}")

    # 加载阈值
    threshold = None
    try:
        # 尝试作为浮点数解析
        threshold = float(args.threshold)
    except ValueError:
        # 作为文件路径读取
        if os.path.exists(args.threshold):
            with open(args.threshold, 'r') as f:
                threshold = float(f.read().strip())
            print(f"Loaded threshold from {args.threshold}: {threshold:.6f}")
        else:
            print(f"Warning: threshold file not found: {args.threshold}")

    # 导出 ONNX
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    export_onnx(model, config['model']['input_dim'], output_path, threshold)

    # 验证
    verify_onnx(model, output_path, config['model']['input_dim'])

    # 打印模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}, shape: {[d.dim_value for d in onnx_model.graph.input[0].type.tensor_type.shape.dim]}")
    print(f"Output: {onnx_model.graph.output[0].name}, shape: {[d.dim_value for d in onnx_model.graph.output[0].type.tensor_type.shape.dim]}")
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")

    # 打印 metadata
    if onnx_model.metadata_props:
        print("\nMetadata:")
        for prop in onnx_model.metadata_props:
            print(f"  {prop.key}: {prop.value}")


if __name__ == '__main__':
    main()