/**
 * AI Integration Test
 *
 * 测试 AI 智能面的完整流程:
 * 1. 加载 ONNX 模型
 * 2. 启动智能面线程
 * 3. 推送 TCPSample 数据
 * 4. 检查 AIAction 输出
 */

#include "neustack/ai/intelligence_plane.hpp"
#include "neustack/common/log.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace neustack;

int main() {
    std::cout << "=== AI Integration Test ===" << std::endl;

    // 创建数据通道
    MetricsBuffer<TCPSample, 1024> metrics_buf;
    SPSCQueue<AIAction, 16> action_queue;

    // 配置模型路径
    IntelligencePlaneConfig config;
    config.orca_model_path = "../models/orca_actor.onnx";
    config.anomaly_model_path = "../models/anomaly_detector.onnx";
    config.bandwidth_model_path = "../models/bandwidth_predictor.onnx";
    config.orca_interval = std::chrono::milliseconds(100);      // 放慢方便观察
    config.anomaly_interval = std::chrono::milliseconds(500);
    config.bandwidth_interval = std::chrono::milliseconds(200);

    std::cout << "Loading models..." << std::endl;

    // 创建并启动智能面
    IntelligencePlane ai(metrics_buf, action_queue, config);
    ai.start();

    std::cout << "Intelligence plane started, pushing samples..." << std::endl;

    // 推送一些模拟的 TCPSample
    // 场景1: 正常流量 (前10个)
    // 场景2: 拥塞 (后10个) - RTT升高，丢包增加，吞吐下降
    for (int i = 0; i < 20; i++) {
        TCPSample sample{};
        sample.timestamp_us = i * 10000;  // 每 10ms 一个样本
        sample.min_rtt_us = 18000;
        sample.srtt_us = 19000;

        if (i < 10) {
            // 正常流量
            sample.rtt_us = 20000 + (i % 3) * 1000;  // 20-22ms RTT
            sample.delivery_rate = 50000000;  // 50 MB/s 稳定
            sample.cwnd = 100;
            sample.bytes_in_flight = 80000;
            sample.packets_sent = 200;
            sample.packets_lost = 0;  // 无丢包
        } else {
            // 拥塞场景
            sample.rtt_us = 50000 + (i % 5) * 5000;  // 50-70ms RTT (升高)
            sample.delivery_rate = 20000000 - (i - 10) * 2000000;  // 20→10 MB/s 下降
            sample.cwnd = 30;
            sample.bytes_in_flight = 60000;
            sample.packets_sent = 200;
            sample.packets_lost = static_cast<uint16_t>(10 + (i - 10) * 2);  // 5-10% 丢包
        }

        metrics_buf.push(sample);

        // 每推送几个样本就等一下
        if (i % 5 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    std::cout << "Pushed 20 samples, waiting for AI decisions..." << std::endl;

    // 等待 AI 处理
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 检查输出的 AIAction
    std::cout << "\n=== AI Actions ===" << std::endl;
    int action_count = 0;
    AIAction action;
    while (action_queue.try_pop(action)) {
        action_count++;
        switch (action.type) {
            case AIAction::Type::CWND_ADJUST:
                std::cout << "  [CWND] alpha=" << action.cwnd.alpha << std::endl;
                break;
            case AIAction::Type::ANOMALY_ALERT:
                std::cout << "  [ANOMALY] score=" << action.anomaly.score << std::endl;
                break;
            case AIAction::Type::BW_PREDICTION:
                std::cout << "  [BW_PRED] predicted=" << action.bandwidth.predicted_bw
                          << " bytes/s" << std::endl;
                break;
            default:
                std::cout << "  [UNKNOWN]" << std::endl;
        }
    }

    std::cout << "\nTotal actions received: " << action_count << std::endl;

    // 停止智能面
    ai.stop();

    std::cout << "\n=== Test Complete ===" << std::endl;
    return action_count > 0 ? 0 : 1;  // 如果收到了 action 就算成功
}
