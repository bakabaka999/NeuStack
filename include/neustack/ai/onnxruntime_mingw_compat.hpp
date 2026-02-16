#ifndef NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP
#define NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP

/**
 * MinGW 兼容层 for ONNX Runtime C API headers
 *
 * 核心问题:
 *   onnxruntime_c_api.h 在 _WIN32 下定义 ORT_API_CALL 为 _stdcall (单下划线),
 *   这是 MSVC 扩展。MinGW GCC 只认 __stdcall (双下划线), 不认 _stdcall,
 *   导致所有函数指针声明解析失败。
 *
 *   同时, SAL2 注解 (_Check_return_, _Ret_maybenull_ 等) 在 _WIN32 路径下
 *   假设由 Windows SDK 提供, 但 MinGW 的 SDK 可能缺少部分扩展注解。
 *
 * 方案:
 *   在 include ONNX Runtime 头文件之前 include 此文件。
 */

#if defined(__MINGW32__) || defined(__MINGW64__)

// ─── 调用约定兼容 ───
// _stdcall 是 MSVC 扩展, MinGW GCC 不认识, 映射到 GCC 认识的 __stdcall
#ifndef _stdcall
#define _stdcall __stdcall
#endif

// ─── SAL (Source Annotation Language) 空实现 ───
// MSVC 用这些做静态分析, MinGW 不需要, 定义为空即可

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

#endif // __MINGW32__ || __MINGW64__

#endif // NEUSTACK_ONNXRUNTIME_MINGW_COMPAT_HPP
