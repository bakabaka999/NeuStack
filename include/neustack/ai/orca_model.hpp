#ifndef NEUSTACK_AI_ORCA_MODEL_HPP
#define NEUSTACK_AI_ORCA_MODEL_HPP

#include "neustack/ai/ai_model.hpp"
#include "neustack/ai/onnx_inference.hpp"
#include <memory>

namespace neustack {

/**
 * Orca 拥塞控制模型
 *
 * 基于 NSDI 2022 论文 "Orca: Pragmatic Learning-based Congestion Control"
 * 使用 DDPG 强化学习，输出 cwnd 调整因子
 */
class OrcaModel : public ICongestionModel {
public:
    /**
     * 构造 Orca 模型
     * @param model_path Actor 网络的 ONNX 模型路径
     */
    explicit OrcaModel(const std::string& model_path);

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char* name() const override { return "Orca"; }

    // ICongestionModel 接口
    std::optional<Output> infer(const Input& input) override;

private:
    std::unique_ptr<ONNXInference> _inference;
};

} // namespace neustack


#endif // NEUSTACK_AI_ORCA_MODEL_HPP