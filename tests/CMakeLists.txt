# tests/CMakeLists.txt

# 引入 Catch2 的辅助模块以启用 catch_discover_tests
include(Catch)

# --- 宏定义：简化测试用例添加 ---
# 参数 name: 可执行文件名
# 参数 source: 源文件路径（相对于当前目录）
macro(neustack_add_test name source)
    add_executable(${name} ${source})

    # 链接 NeuStack 核心库和 Catch2 运行时
    target_link_libraries(${name}
        PRIVATE
            neustack_lib
            Catch2::Catch2WithMain
    )

    # 启用 C++20 标准（继承自 neustack_lib 或在此显式指定）
    target_compile_features(${name} PRIVATE cxx_std_20)

    # 自动注册测试用例到 ctest
    catch_discover_tests(${name})
endmacro()

# --- 基础通用组件测试 (common/) ---
neustack_add_test(test_checksum    unit/common/test_checksum.cpp)
neustack_add_test(test_ip_addr     unit/common/test_ip_addr.cpp)
neustack_add_test(test_ring_buffer unit/common/test_ring_buffer.cpp)
neustack_add_test(test_spsc_queue  unit/common/test_spsc_queue.cpp)
neustack_add_test(test_memory_pool unit/common/test_memory_pool.cpp)

# --- 传输层测试 (transport/) ---
neustack_add_test(test_tcp_segment unit/transport/test_tcp_segment.cpp)
neustack_add_test(test_tcp_seq     unit/transport/test_tcp_seq.cpp)
neustack_add_test(test_tcp_cubic   unit/transport/test_tcp_cubic.cpp)
neustack_add_test(test_tcp_reno    unit/transport/test_tcp_reno.cpp)
neustack_add_test(test_tcp_orca    unit/transport/test_tcp_orca.cpp)
neustack_add_test(test_tcp_builder unit/transport/test_tcp_builder.cpp)
neustack_add_test(test_tcp_sample  unit/transport/test_tcp_sample.cpp)
neustack_add_test(test_tcp_edge    unit/transport/test_tcp_edge.cpp)
neustack_add_test(test_udp_edge    unit/transport/test_udp_edge.cpp)

# --- 网络层测试 (net/) ---
neustack_add_test(test_ip_edge     unit/net/test_ip_edge.cpp)

# --- 应用层测试 (app/) ---
neustack_add_test(test_http_types  unit/app/test_http_types.cpp)
neustack_add_test(test_http_parser unit/app/test_http_parser.cpp)
neustack_add_test(test_http_edge   unit/app/test_http_edge.cpp)

# --- 指标测试 (metrics/) ---
neustack_add_test(test_global_metrics unit/metrics/test_global_metrics.cpp)

# --- 防火墙测试 (firewall/) ---
neustack_add_test(test_rule_engine  unit/firewall/test_rule_engine.cpp)
neustack_add_test(test_rate_limiter unit/firewall/test_rate_limiter.cpp)
neustack_add_test(test_firewall_ai  unit/firewall/test_firewall_ai.cpp)

# --- 安全模型测试 (security/) ---
neustack_add_test(test_security_model unit/ai/test_security_model.cpp)

# --- Telemetry测试（telemetry/）---
neustack_add_test(test_metrics_registry unit/telemetry/test_metrics_registry.cpp)

# --- 集成测试（不需要 ONNX）---
add_executable(test_tcp_handshake integration/test_tcp_handshake.cpp)
target_link_libraries(test_tcp_handshake PRIVATE neustack_lib)
add_test(NAME "Integration: TCP handshake and echo" COMMAND test_tcp_handshake)

add_executable(test_http_roundtrip integration/test_http_roundtrip.cpp)
target_link_libraries(test_http_roundtrip PRIVATE neustack_lib)
add_test(NAME "Integration: HTTP request/response roundtrip" COMMAND test_http_roundtrip)

add_executable(test_tcp_stress integration/test_tcp_stress.cpp)
target_link_libraries(test_tcp_stress PRIVATE neustack_lib)
add_test(NAME "Integration: TCP connection stress limits" COMMAND test_tcp_stress)

add_executable(test_firewall_integration integration/test_firewall_integration.cpp)
target_link_libraries(test_firewall_integration PRIVATE neustack_lib)
add_test(NAME "Integration: Firewall packet filtering" COMMAND test_firewall_integration)

add_executable(test_firewall_ai_shadow integration/test_firewall_ai_shadow.cpp)
target_link_libraries(test_firewall_ai_shadow PRIVATE neustack_lib)
add_test(NAME "Integration: Firewall AI Shadow Mode" COMMAND test_firewall_ai_shadow)

add_executable(test_firewall_e2e integration/test_firewall_e2e.cpp)
target_link_libraries(test_firewall_e2e PRIVATE neustack_lib)
add_test(NAME "Integration: Firewall E2E full feature" COMMAND test_firewall_e2e)

# --- 性能测试---
add_executable(bench_checksum benchmark/bench_checksum.cpp)
target_link_libraries(bench_checksum PRIVATE neustack_lib)
add_test(NAME "Benchmark: Checksum calculate" COMMAND bench_checksum)

add_executable(bench_spsc_queue benchmark/bench_spsc_queue.cpp)
target_link_libraries(bench_spsc_queue PRIVATE neustack_lib)
add_test(NAME "Benchmark: Spsc queue test" COMMAND bench_spsc_queue)

add_executable(bench_tcp_throughput benchmark/bench_tcp_throughput.cpp)
target_link_libraries(bench_tcp_throughput PRIVATE neustack_lib)
add_test(NAME "Benchmark: TCP throughput test" COMMAND bench_tcp_throughput)

add_executable(bench_tcp_conn benchmark/bench_tcp_conn.cpp)
target_link_libraries(bench_tcp_conn PRIVATE neustack_lib)
add_test(NAME "Benchmark: TCP connection test" COMMAND bench_tcp_conn)

add_executable(bench_http benchmark/bench_http.cpp)
target_link_libraries(bench_http PRIVATE neustack_lib)
add_test(NAME "Benchmark: HTTP test" COMMAND bench_http)

add_executable(bench_memory_pool benchmark/bench_memory_pool.cpp)
target_link_libraries(bench_memory_pool PRIVATE neustack_lib)
add_test(NAME "Benchmark: Memory pool test" COMMAND bench_memory_pool)

add_executable(bench_metrics_hotpath benchmark/bench_metrics_hotpath.cpp)
target_link_libraries(bench_metrics_hotpath PRIVATE neustack_lib)
add_test(NAME "Benchmark: Metrics hotpath" COMMAND bench_metrics_hotpath)

# --- AI 智能平面相关测试 ---
# 仅在 NEUSTACK_ENABLE_AI 选项开启时编译
if(NEUSTACK_ENABLE_AI)
    message(STATUS "NeuStack: AI features enabled for unit testing.")

    # 特征提取模块测试
    neustack_add_test(test_ai_features unit/ai/test_ai_features.cpp)

    # 智能 Agent 状态机测试
    neustack_add_test(test_ai_agent unit/ai/test_ai_agent.cpp)

    # 为 AI 相关测试添加宏定义 NEUSTACK_AI_ENABLED
    # 这样代码中可以使用 #ifdef NEUSTACK_AI_ENABLED
    target_compile_definitions(test_ai_features PRIVATE NEUSTACK_AI_ENABLED)
    target_compile_definitions(test_ai_agent    PRIVATE NEUSTACK_AI_ENABLED)

    # --- 集成测试（需要 ONNX Runtime 和模型文件）---
    if(ONNXRUNTIME_FOUND)
        # ONNX Runtime 基础验证
        add_executable(test_onnx_runtime integration/test_onnx_runtime.cpp)
        target_include_directories(test_onnx_runtime PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
        target_link_libraries(test_onnx_runtime PRIVATE neustack_lib ${ONNXRUNTIME_LIBRARIES})

        # AI 模型完整管线测试
        add_executable(test_ai_models integration/test_ai_models.cpp)
        target_link_libraries(test_ai_models PRIVATE neustack_lib)

        # RPATH
        if(APPLE)
            set_target_properties(test_onnx_runtime test_ai_models PROPERTIES
                BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-arm64/lib;${CMAKE_SOURCE_DIR}/third_party/onnxruntime/macos-x64/lib"
            )
        elseif(UNIX)
            set_target_properties(test_onnx_runtime test_ai_models PROPERTIES
                BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/linux-x64/lib"
            )
        endif()
    endif()
endif()
