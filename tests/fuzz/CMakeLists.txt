add_library(neustack_fuzz_options INTERFACE)
target_compile_features(neustack_fuzz_options INTERFACE cxx_std_20)

# 根 CMakeLists 已验证 Clang，这里不再用 check_cxx_compiler_flag
# （该宏会尝试链接，而 -fsanitize=fuzzer-no-link 插桩后链接必失败）

if(APPLE)
    # macOS: Clang 不自动链接 libFuzzer，需要手动找到静态库
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
        OUTPUT_VARIABLE NEUSTACK_CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(NEUSTACK_LIBFUZZER_RUNTIME "")
    foreach(candidate
            "${NEUSTACK_CLANG_RESOURCE_DIR}/lib/darwin/libclang_rt.fuzzer_osx.a"
            "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a"
            "/opt/homebrew/opt/llvm/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a")
        file(GLOB NEUSTACK_LIBFUZZER_CANDIDATES "${candidate}")
        if(NEUSTACK_LIBFUZZER_CANDIDATES)
            list(GET NEUSTACK_LIBFUZZER_CANDIDATES 0 NEUSTACK_LIBFUZZER_RUNTIME)
            break()
        endif()
    endforeach()

    if(NOT EXISTS "${NEUSTACK_LIBFUZZER_RUNTIME}")
        message(FATAL_ERROR
            "libFuzzer runtime not found on this macOS toolchain. "
            "Install full Xcode or a Clang/LLVM distribution that ships "
            "libclang_rt.fuzzer_osx.a, then re-run CMake.")
    endif()

    target_compile_options(neustack_fuzz_options INTERFACE
        -fno-omit-frame-pointer
        -fsanitize=fuzzer-no-link
    )
    target_link_libraries(neustack_fuzz_options INTERFACE "${NEUSTACK_LIBFUZZER_RUNTIME}")
else()
    # Linux: -fsanitize=fuzzer 同时处理编译插桩和链接
    target_compile_options(neustack_fuzz_options INTERFACE
        -fno-omit-frame-pointer
        -fsanitize=fuzzer
    )
    target_link_options(neustack_fuzz_options INTERFACE -fsanitize=fuzzer)
endif()

set(NEUSTACK_FUZZ_CORPUS_DIR "${CMAKE_BINARY_DIR}/fuzz-corpus")
set(NEUSTACK_FUZZ_DURATION 10 CACHE STRING "Fuzz test duration in seconds for CTest")

function(neustack_add_fuzzer name source corpus_subdir)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE neustack_lib neustack_fuzz_options)

    file(MAKE_DIRECTORY "${NEUSTACK_FUZZ_CORPUS_DIR}/${corpus_subdir}")

    # 注册到 ctest：跑 NEUSTACK_FUZZ_DURATION 秒，限制输入 4KB，无 stdout 噪音
    add_test(
        NAME "Fuzz: ${name}"
        COMMAND ${name}
            "${NEUSTACK_FUZZ_CORPUS_DIR}/${corpus_subdir}"
            -max_total_time=${NEUSTACK_FUZZ_DURATION}
            -max_len=4096
            -print_final_stats=1
            -close_fd_mask=1
    )
    set_tests_properties("Fuzz: ${name}" PROPERTIES
        TIMEOUT ${NEUSTACK_FUZZ_DURATION}0   # 10x duration 作为硬超时
        LABELS "fuzz"
    )
endfunction()

neustack_add_fuzzer(fuzz_http_parser http_parser_fuzzer.cpp http)
neustack_add_fuzzer(fuzz_dns_parser  dns_parser_fuzzer.cpp  dns)
neustack_add_fuzzer(fuzz_ipv4_parser ipv4_parser_fuzzer.cpp ipv4)
neustack_add_fuzzer(fuzz_tcp_parser  tcp_parser_fuzzer.cpp  tcp)

message(STATUS "NeuStack fuzz corpus dir: ${NEUSTACK_FUZZ_CORPUS_DIR}")
message(STATUS "NeuStack fuzz duration:   ${NEUSTACK_FUZZ_DURATION}s per target")
