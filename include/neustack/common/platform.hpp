#ifndef NEUSTACK_COMMON_PLATFORM_HPP
#define NEUSTACK_COMMON_PLATFORM_HPP

/**
 * 跨平台字节序 & 基础类型
 *
 * Windows: 自行实现 htons/ntohs/htonl/ntohl，不引入 winsock2.h/windows.h，
 *          避免 DELETE / IN / ERROR 等宏污染 enum 定义。
 *          真正需要 socket API 的地方（hal_windows.cpp）自行 include winsock2.h。
 *
 * Linux/macOS: 直接用系统头文件。
 */

#include <cstdint>
#include <cstddef>

#ifdef _WIN32
    // ─── Windows: 自行实现，不引入系统网络头 ───

    // ssize_t
    #ifdef _MSC_VER
        #include <BaseTsd.h>
        typedef SSIZE_T ssize_t;
    #endif
    // MinGW 通常已定义 ssize_t，不需要额外处理

    // 字节序交换 (compiler builtins)
    namespace neustack::detail {

    inline uint16_t byte_swap16(uint16_t val) {
    #if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap16(val);
    #elif defined(_MSC_VER)
        return _byteswap_ushort(val);
    #else
        return (val << 8) | (val >> 8);
    #endif
    }

    inline uint32_t byte_swap32(uint32_t val) {
    #if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap32(val);
    #elif defined(_MSC_VER)
        return _byteswap_ulong(val);
    #else
        return ((val & 0xFF000000) >> 24) |
               ((val & 0x00FF0000) >> 8)  |
               ((val & 0x0000FF00) << 8)  |
               ((val & 0x000000FF) << 24);
    #endif
    }

    } // namespace neustack::detail

    // 字节序转换 (替代 winsock2.h 提供的版本)
    // x86/x64/ARM (LE) 都是 little-endian → 需要 swap 为网络字节序 (big-endian)
    #ifndef htons
    inline uint16_t htons(uint16_t val) { return neustack::detail::byte_swap16(val); }
    #endif
    #ifndef ntohs
    inline uint16_t ntohs(uint16_t val) { return neustack::detail::byte_swap16(val); }
    #endif
    #ifndef htonl
    inline uint32_t htonl(uint32_t val) { return neustack::detail::byte_swap32(val); }
    #endif
    #ifndef ntohl
    inline uint32_t ntohl(uint32_t val) { return neustack::detail::byte_swap32(val); }
    #endif

#else
    // ─── Linux / macOS: 用系统头文件 ───
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

#endif // NEUSTACK_COMMON_PLATFORM_HPP
