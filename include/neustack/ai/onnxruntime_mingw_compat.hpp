#ifndef NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP
#define NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP

/**
 * MinGW 兼容层 for ONNX Runtime C API headers
 *
 * 问题: onnxruntime_c_api.h 使用了 MSVC 独有的 SAL 注解 (_In_, _Out_, etc.)
 *       和 ORT_API_CALL 宏。在 MinGW (定义 _WIN32 但没有 _MSC_VER) 下
 *       这些宏可能未定义或定义不正确，导致编译失败。
 *
 * 方案: 在 include ONNX Runtime 头文件之前，先 include 此文件，
 *       将所有缺失的 SAL 注解定义为空。
 */

#if defined(__MINGW32__) || defined(__MINGW64__)

// ─── SAL (Source Annotation Language) 空实现 ───
// MSVC 用这些做静态分析，MinGW 不需要

#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif

#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(x)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(x)
#endif
#ifndef _Out_writes_to_
#define _Out_writes_to_(x, y)
#endif

#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif

#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif

#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif

#ifndef _Check_return_
#define _Check_return_
#endif

#ifndef _Success_
#define _Success_(x)
#endif

// ─── ORT_API_CALL: 在 MinGW 下使用 __stdcall ───
// ONNX Runtime 在 _WIN32 + _MSC_VER 下定义为 __stdcall
// MinGW GCC 也支持 __stdcall，但 onnxruntime header 可能检测不到

#ifndef ORT_API_CALL
#define ORT_API_CALL __stdcall
#endif

// ─── NO_EXCEPTION ───
#ifndef NO_EXCEPTION
#ifdef __cplusplus
#define NO_EXCEPTION noexcept
#else
#define NO_EXCEPTION
#endif
#endif

// ─── ORT_MUST_USE_RESULT ───
#ifndef ORT_MUST_USE_RESULT
#if __has_cpp_attribute(nodiscard)
#define ORT_MUST_USE_RESULT [[nodiscard]]
#else
#define ORT_MUST_USE_RESULT
#endif
#endif

// ─── ORT_ALL_ARGS_NONNULL ───
#ifndef ORT_ALL_ARGS_NONNULL
#define ORT_ALL_ARGS_NONNULL __attribute__((nonnull))
#endif

// ─── ORT_EXPORT ───
#ifndef ORT_EXPORT
#define ORT_EXPORT __attribute__((visibility("default")))
#endif

#endif // __MINGW32__ || __MINGW64__

#endif // NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP
