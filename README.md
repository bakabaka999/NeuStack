# NeuStack

**跨平台用户态 TCP/IP 协议栈，集成 AI 拥塞控制**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-macOS%20|%20Linux%20|%20Windows-lightgrey.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)]()

## 项目简介

NeuStack 是一个从零实现的、高性能的、模块化的用户态 TCP/IP 协议栈。主要特性：

- **跨平台 HAL**：通过硬件抽象层支持 macOS (utun) / Linux (TAP) / Windows (Wintun)
- **完整协议栈**：IPv4、ICMP、ARP、UDP、TCP
- **AI 拥塞控制**：基于强化学习 (PPO) 的智能 CWND 调节
- **用户态实现**：便于调试、定制和部署

## 快速开始

### 依赖

- CMake ≥ 3.20
- C++17 兼容编译器 (Clang/GCC/MSVC)
- Ninja (推荐)

**可选依赖：**
- ONNX Runtime (AI 拥塞控制)
- Catch2 (单元测试)

### 构建

```bash
# 配置
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 编译
cmake --build build

# 运行 (需要 root 权限创建虚拟网卡)
sudo ./build/neustack
```

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `NEUSTACK_BUILD_TESTS` | ON | 编译单元测试 |
| `NEUSTACK_BUILD_EXAMPLES` | ON | 编译示例程序 |
| `NEUSTACK_ENABLE_ASAN` | OFF | 启用 Address Sanitizer |
| `NEUSTACK_ENABLE_AI` | OFF | 启用 AI 拥塞控制 (需要 ONNX Runtime) |

```bash
# 示例：启用 Address Sanitizer
cmake -B build -DNEUSTACK_ENABLE_ASAN=ON
```

## 项目结构

```
NeuStack/
├── CMakeLists.txt          # 主构建脚本
├── cmake/                  # CMake 模块
│   └── Platform.cmake      # 平台检测
├── include/neustack/       # 公共头文件
│   ├── common/             # 通用工具 (buffer, checksum, endian)
│   ├── hal/                # 硬件抽象层接口
│   ├── net/                # 网络层 (IPv4, ICMP, ARP)
│   ├── transport/          # 传输层 (TCP, UDP)
│   └── ai/                 # AI 拥塞控制
├── src/                    # 源代码实现
│   ├── hal/                # 平台特定 HAL 实现
│   ├── net/
│   ├── transport/
│   └── ai/
├── tests/                  # 测试代码
├── examples/               # 示例程序
├── models/                 # AI 模型文件 (.onnx)
├── scripts/                # 辅助脚本
└── docs/                   # 文档
```

## 开发路线

- [x] Phase 1: 项目骨架 + HAL
- [ ] Phase 2: IPv4 解析/发送
- [ ] Phase 3: ICMP (ping)
- [ ] Phase 4: ARP
- [ ] Phase 5: UDP
- [ ] Phase 6: TCP
- [ ] Phase 7: AI 拥塞控制
- [ ] Phase 8: Socket API + 跨平台

## 文档

- [项目白皮书](docs/project_whitepaper.md) - 详细设计文档

## 许可证

MIT License
