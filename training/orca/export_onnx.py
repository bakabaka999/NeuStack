import os
import yaml
import argparse
import torch
import torch.nn as nn
import onnx
import onnxruntime as ort
import numpy as np

from network import Actor, OrcaActor
from sac import SACActor


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


class DeterministicSACWrapper(nn.Module):
    """将 SACActor 包装成确定性输出: tanh(mean)"""
    def __init__(self, actor: SACActor):
        super().__init__()
        self.actor = actor

    def forward(self, state):
        return self.actor.get_deterministic_action(state)


def export_onnx(model, state_dim: int, output_path: str):
    """导出 Actor 到 ONNX"""
    model.eval()

    dummy_input = torch.randn(1, state_dim)

    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['state'],
        output_names=['alpha'],
        dynamic_axes={
            'state': {0: 'batch'},
            'alpha': {0: 'batch'}
        },
        opset_version=11,
        do_constant_folding=True
    )

    print(f"Exported to {output_path}")

    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")


def verify_onnx(pytorch_model, onnx_path: str, state_dim: int):
    """验证 ONNX 与 PyTorch 输出一致"""
    pytorch_model.eval()

    test_input = np.random.randn(5, state_dim).astype(np.float32)

    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'state': test_input})[0]

    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max difference: {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED!")
    else:
        print("WARNING: Outputs differ!")


def detect_checkpoint_type(checkpoint: dict) -> str:
    """检测 checkpoint 是 SAC 还是 DDPG"""
    actor_keys = checkpoint['actor'].keys()
    if 'mean_head.weight' in actor_keys:
        return 'sac'
    return 'ddpg'


def main():
    parser = argparse.ArgumentParser(description='Export Orca model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/final_model.pth')
    parser.add_argument('--lightweight', action='store_true', help='Export lightweight model')
    args = parser.parse_args()

    config = load_config(args.config)

    checkpoint = torch.load(args.checkpoint, map_location='cpu', weights_only=False)
    algo = detect_checkpoint_type(checkpoint)
    print(f"Detected checkpoint type: {algo.upper()}")

    state_dim = config['agent']['state_dim']
    action_dim = config['agent']['action_dim']
    hidden_dims = config['agent']['hidden_dims']

    if algo == 'sac':
        actor = SACActor(state_dim, hidden_dims, action_dim)
        actor.load_state_dict(checkpoint['actor'])
        model = DeterministicSACWrapper(actor)
    else:
        model = Actor(state_dim, hidden_dims, action_dim)
        model.load_state_dict(checkpoint['actor'])

    model.eval()
    print(f"Loaded model from {args.checkpoint}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    export_onnx(model, state_dim, output_path)
    verify_onnx(model, output_path, state_dim)

    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")

    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}")
    print(f"Output: {onnx_model.graph.output[0].name}")


if __name__ == '__main__':
    main()
