#ifndef NEUSTACK_COMMON_PLATFORM_HPP
#define NEUSTACK_COMMON_PLATFORM_HPP

/**
 * 跨平台网络字节序 & socket 头文件
 *
 * 统一封装 htons/ntohs/htonl/ntohl 等函数，
 * 避免在每个文件中处理平台差异。
 */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    // winsock2.h 提供 htons/ntohs/htonl/ntohl/inet_addr 等

    // windows.h 定义了 #define ERROR 0, 与 LogLevel::ERROR 冲突
    #ifdef ERROR
    #undef ERROR
    #endif

    // MinGW 通常已定义 ssize_t, MSVC 没有
    #ifdef _MSC_VER
        #include <BaseTsd.h>
        typedef SSIZE_T ssize_t;
    #endif
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

#endif // NEUSTACK_COMMON_PLATFORM_HPP
