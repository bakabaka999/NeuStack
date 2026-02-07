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

# --- 基础通用组件测试 ---
neustack_add_test(test_checksum    unit/test_checksum.cpp)
neustack_add_test(test_ip_addr     unit/test_ip_addr.cpp)
neustack_add_test(test_tcp_segment unit/test_tcp_segment.cpp)
neustack_add_test(test_ring_buffer unit/test_ring_buffer.cpp)
neustack_add_test(test_spsc_queue  unit/test_spsc_queue.cpp)

# --- 应用层协议测试 ---
neustack_add_test(test_http_types  unit/test_http_types.cpp)
neustack_add_test(test_http_parser unit/test_http_parser.cpp)

# --- AI 智能平面相关测试 ---
# 仅在 NEUSTACK_ENABLE_AI 选项开启时编译
if(NEUSTACK_ENABLE_AI)
    message(STATUS "NeuStack: AI features enabled for unit testing.")

    # 特征提取模块测试
    neustack_add_test(test_ai_features unit/test_ai_features.cpp)
    
    # 智能 Agent 状态机测试
    neustack_add_test(test_ai_agent unit/test_ai_agent.cpp)
    
    # 为 AI 相关测试添加宏定义 NEUSTACK_AI_ENABLED
    # 这样代码中可以使用 #ifdef NEUSTACK_AI_ENABLED
    target_compile_definitions(test_ai_features PRIVATE NEUSTACK_AI_ENABLED)
    target_compile_definitions(test_ai_agent    PRIVATE NEUSTACK_AI_ENABLED)
endif()