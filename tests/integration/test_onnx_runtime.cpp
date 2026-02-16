/**
 * ONNX Runtime 最小测试示例
 *
 * 编译: cmake .. -DNEUSTACK_ENABLE_AI=ON && make onnx_test
 * 运行: ./onnx_test
 */

#include <onnxruntime_cxx_api.h>
#include <vector>
#include <iostream>
#include <string>
#include <cstring>
#include <neustack/common/log.hpp>

int main() {
    std::cout << "=== ONNX Runtime Test ===" << std::endl;

    try {
        // 1. 创建环境 (全局一次)
        std::cout << "[1] Creating Ort::Env..." << std::endl;
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "NeuStackTest");
        std::cout << "    OK" << std::endl;

        // 2. 配置会话
        std::cout << "[2] Configuring session options..." << std::endl;
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        std::cout << "    OK" << std::endl;

        // 3. 加载模型
        std::cout << "[3] Loading model..." << std::endl;
        const char* model_path_str = "models/test_simple.onnx";
#ifdef _WIN32
        // Windows: ORTCHAR_T = wchar_t
        std::wstring wide_path(model_path_str, model_path_str + std::strlen(model_path_str));
        Ort::Session session(env, wide_path.c_str(), session_options);
#else
        Ort::Session session(env, model_path_str, session_options);
#endif
        std::cout << "    Loaded: " << model_path_str << std::endl;

        // 4. 获取输入/输出信息
        std::cout << "[4] Getting model metadata..." << std::endl;
        Ort::AllocatorWithDefaultOptions allocator;

        // 输入名称
        auto input_name = session.GetInputNameAllocated(0, allocator);
        std::cout << "    Input name: " << input_name.get() << std::endl;

        // 输入形状
        auto input_shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        std::cout << "    Input shape: [";
        for (size_t i = 0; i < input_shape.size(); i++) {
            std::cout << input_shape[i] << (i < input_shape.size() - 1 ? ", " : "");
        }
        std::cout << "]" << std::endl;

        // 输出名称
        auto output_name = session.GetOutputNameAllocated(0, allocator);
        std::cout << "    Output name: " << output_name.get() << std::endl;

        // 5. 准备输入数据
        std::cout << "[5] Preparing input data..." << std::endl;
        std::vector<float> input_data = {1.0f, 2.0f, 3.0f};
        std::vector<int64_t> input_shape_vec = {1, 3};

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            input_data.data(),
            input_data.size(),
            input_shape_vec.data(),
            input_shape_vec.size()
        );
        std::cout << "    Input: [" << input_data[0] << ", " << input_data[1] << ", " << input_data[2] << "]" << std::endl;

        // 6. 执行推理
        std::cout << "[6] Running inference..." << std::endl;
        const char* input_names[] = {input_name.get()};
        const char* output_names[] = {output_name.get()};

        auto output_tensors = session.Run(
            Ort::RunOptions{nullptr},
            input_names, &input_tensor, 1,
            output_names, 1
        );
        std::cout << "    OK" << std::endl;

        // 7. 获取输出
        std::cout << "[7] Getting output..." << std::endl;
        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        std::cout << "    Output: " << output_data[0] << std::endl;

        std::cout << std::endl;
        std::cout << "=== Test PASSED ===" << std::endl;
        return 0;

    } catch (const Ort::Exception& e) {
        LOG_ERROR(AI, "ONNX Runtime error: %s", e.what());
        return 1;
    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Error: %s", e.what());
        return 1;
    }
}
