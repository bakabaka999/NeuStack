import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from network import Actor, OrcaActor


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def export_onnx(model, state_dim: int, output_path: str):
    """导出 Actor 到 ONNX"""
    model.eval()

    # 示例输入
    dummy_input = torch.randn(1, state_dim)

    # 导出
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['state'],
        output_names=['action'],
        dynamic_axes={
            'state': {0: 'batch_size'},
            'action': {0: 'batch_size'}
        },
        opset_version=17,
        do_constant_folding=True
    )

    print(f"Exported to {output_path}")

    # 验证
    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")


def verify_onnx(pytorch_model, onnx_path: str, state_dim: int):
    """验证 ONNX 与 PyTorch 输出一致"""
    pytorch_model.eval()

    # 测试输入
    test_input = np.random.randn(5, state_dim).astype(np.float32)

    # PyTorch
    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    # ONNX
    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'state': test_input})[0]

    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max difference: {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED!")
    else:
        print("WARNING: Outputs differ!")


def create_lightweight_model(checkpoint_path: str, config: dict) -> OrcaActor:
    """从完整 checkpoint 创建轻量级 Actor"""
    # 加载完整模型
    checkpoint = torch.load(checkpoint_path, map_location='cpu')

    full_actor = Actor(
        state_dim=config['agent']['state_dim'],
        hidden_dims=config['agent']['hidden_dims'],
        action_dim=config['agent']['action_dim']
    )
    full_actor.load_state_dict(checkpoint['actor'])
    full_actor.eval()

    # 创建轻量级模型并知识蒸馏（简化版：直接用小模型重新训练）
    # 这里简单返回完整模型，实际部署可以训练一个小模型
    return full_actor


def main():
    parser = argparse.ArgumentParser(description='Export Orca model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/final_model.pth')
    parser.add_argument('--lightweight', action='store_true', help='Export lightweight model')
    args = parser.parse_args()

    config = load_config(args.config)

    # 加载模型
    checkpoint = torch.load(args.checkpoint, map_location='cpu')

    if args.lightweight:
        model = OrcaActor(
            state_dim=config['agent']['state_dim'],
            hidden_dim=64
        )
        # 注意：轻量级模型需要单独训练或蒸馏
        print("Warning: Lightweight model needs separate training")
    else:
        model = Actor(
            state_dim=config['agent']['state_dim'],
            hidden_dims=config['agent']['hidden_dims'],
            action_dim=config['agent']['action_dim']
        )
        model.load_state_dict(checkpoint['actor'])

    model.eval()
    print(f"Loaded model from {args.checkpoint}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    # 导出
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    export_onnx(model, config['agent']['state_dim'], output_path)
    verify_onnx(model, output_path, config['agent']['state_dim'])

    # 打印模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")

    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}")
    print(f"Output: {onnx_model.graph.output[0].name}")


if __name__ == '__main__':
    main()
