/**
 * @file hal_windows.cpp
 * @brief Windows HAL implementation using Wintun
 *
 * Wintun 是 WireGuard 项目的轻量级 L3 TUN 驱动。
 * 通过运行时加载 wintun.dll, 使用 ring buffer API 收发 IP 包。
 *
 * 参考: https://www.wintun.net/
 */

#ifdef NEUSTACK_PLATFORM_WINDOWS

#include "neustack/hal/hal_windows.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// windows.h 重新污染的宏，必须清理
#ifdef ERROR
#undef ERROR
#endif
#ifdef DELETE
#undef DELETE
#endif
#ifdef IN
#undef IN
#endif

using namespace neustack;

// ─── Wintun 函数签名 (与 wintun.h 一致) ───

using WintunCreateAdapterFunc = void *(__stdcall *)(
    const wchar_t *, const wchar_t *, const void *);
using WintunCloseAdapterFunc = void(__stdcall *)(void *);
using WintunStartSessionFunc = void *(__stdcall *)(void *, unsigned int);
using WintunEndSessionFunc = void(__stdcall *)(void *);
using WintunGetReadWaitEventFunc = void *(__stdcall *)(void *);
using WintunReceivePacketFunc = uint8_t *(__stdcall *)(void *, unsigned int *);
using WintunReleaseReceivePacketFunc = void(__stdcall *)(void *, const uint8_t *);
using WintunAllocateSendPacketFunc = uint8_t *(__stdcall *)(void *, unsigned int);
using WintunSendPacketFunc = void(__stdcall *)(void *, const uint8_t *);

// ─── DLL 加载 ───

bool WindowsDevice::load_wintun_dll()
{
    _wintun_dll = (void *)LoadLibraryA("wintun.dll");
    if (!_wintun_dll) {
        LOG_ERROR(HAL, "Failed to load wintun.dll (error=%lu)", GetLastError());
        LOG_ERROR(HAL, "  Download from https://www.wintun.net/");
        return false;
    }

    auto resolve = [&](const char *name) -> void * {
        void *fn = (void *)GetProcAddress((HMODULE)_wintun_dll, name);
        if (!fn) {
            LOG_ERROR(HAL, "Failed to resolve %s from wintun.dll", name);
        }
        return fn;
    };

    _fn_create_adapter         = resolve("WintunCreateAdapter");
    _fn_close_adapter          = resolve("WintunCloseAdapter");
    _fn_start_session          = resolve("WintunStartSession");
    _fn_end_session            = resolve("WintunEndSession");
    _fn_get_read_wait_event    = resolve("WintunGetReadWaitEvent");
    _fn_receive_packet         = resolve("WintunReceivePacket");
    _fn_release_receive_packet = resolve("WintunReleaseReceivePacket");
    _fn_allocate_send_packet   = resolve("WintunAllocateSendPacket");
    _fn_send_packet            = resolve("WintunSendPacket");

    if (!_fn_create_adapter || !_fn_close_adapter ||
        !_fn_start_session || !_fn_end_session ||
        !_fn_get_read_wait_event || !_fn_receive_packet ||
        !_fn_release_receive_packet || !_fn_allocate_send_packet ||
        !_fn_send_packet) {
        FreeLibrary((HMODULE)_wintun_dll);
        _wintun_dll = nullptr;
        return false;
    }

    LOG_DEBUG(HAL, "wintun.dll loaded successfully");
    return true;
}

// ─── NetDevice 接口实现 ───

int WindowsDevice::open()
{
    // 1. 加载 wintun.dll
    if (!load_wintun_dll()) {
        return -1;
    }

    // 2. 创建 adapter
    auto create = (WintunCreateAdapterFunc)_fn_create_adapter;
    _adapter = create(L"NeuStack", L"NeuStack", nullptr);
    if (!_adapter) {
        LOG_ERROR(HAL, "WintunCreateAdapter failed (error=%lu)", GetLastError());
        FreeLibrary((HMODULE)_wintun_dll);
        _wintun_dll = nullptr;
        return -1;
    }

    // 3. 启动 session (ring buffer)
    auto start = (WintunStartSessionFunc)_fn_start_session;
    _session = start(_adapter, RING_CAPACITY);
    if (!_session) {
        LOG_ERROR(HAL, "WintunStartSession failed (error=%lu)", GetLastError());
        auto close_adapter = (WintunCloseAdapterFunc)_fn_close_adapter;
        close_adapter(_adapter);
        _adapter = nullptr;
        FreeLibrary((HMODULE)_wintun_dll);
        _wintun_dll = nullptr;
        return -1;
    }

    // 4. 获取读等待事件句柄 (用于超时等待)
    auto get_event = (WintunGetReadWaitEventFunc)_fn_get_read_wait_event;
    _read_event = get_event(_session);

    _name = "NeuStack";
    LOG_DEBUG(HAL, "Wintun adapter opened: %s (ring=%u bytes)", _name.c_str(), RING_CAPACITY);
    return 0;
}

int WindowsDevice::close()
{
    if (_session) {
        auto end = (WintunEndSessionFunc)_fn_end_session;
        end(_session);
        _session = nullptr;
        _read_event = nullptr;
    }
    if (_adapter) {
        auto close_adapter = (WintunCloseAdapterFunc)_fn_close_adapter;
        close_adapter(_adapter);
        _adapter = nullptr;
    }
    if (_wintun_dll) {
        FreeLibrary((HMODULE)_wintun_dll);
        _wintun_dll = nullptr;
    }

    LOG_DEBUG(HAL, "Wintun adapter closed");
    _name.clear();
    return 0;
}

ssize_t WindowsDevice::send(const uint8_t *data, size_t len)
{
    if (!_session) return -1;

    // Wintun: 直接写裸 IP 包, 无前缀 (与 Linux TUN + IFF_NO_PI 一致)
    auto allocate = (WintunAllocateSendPacketFunc)_fn_allocate_send_packet;
    uint8_t *packet = allocate(_session, static_cast<unsigned int>(len));
    if (!packet) {
        LOG_ERROR(HAL, "WintunAllocateSendPacket failed (error=%lu, len=%zu)",
                  GetLastError(), len);
        return -1;
    }

    std::memcpy(packet, data, len);

    auto send_pkt = (WintunSendPacketFunc)_fn_send_packet;
    send_pkt(_session, packet);

    LOG_TRACE(HAL, "sent %zu bytes", len);
    return static_cast<ssize_t>(len);
}

ssize_t WindowsDevice::recv(uint8_t *buf, size_t len, int timeout_ms)
{
    if (!_session) return -1;

    auto receive = (WintunReceivePacketFunc)_fn_receive_packet;
    auto release = (WintunReleaseReceivePacketFunc)_fn_release_receive_packet;

    // Wintun: 直接读裸 IP 包, 无前缀
    unsigned int packet_size = 0;
    uint8_t *packet = receive(_session, &packet_size);

    if (!packet) {
        DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_ITEMS) {
            LOG_ERROR(HAL, "WintunReceivePacket failed (error=%lu)", err);
            return -1;
        }

        // 没有数据, 使用事件等待 (类似 poll)
        if (timeout_ms == 0) return 0;

        DWORD wait_ms = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
        DWORD result = WaitForSingleObject(_read_event, wait_ms);
        if (result != WAIT_OBJECT_0) {
            return 0;  // 超时
        }

        // 重试接收
        packet = receive(_session, &packet_size);
        if (!packet) return 0;  // 竞争条件, 正常
    }

    // 拷贝到用户缓冲区
    size_t copy_len = std::min(static_cast<size_t>(packet_size), len);
    std::memcpy(buf, packet, copy_len);

    // 释放 ring buffer 中的包
    release(_session, packet);

    LOG_TRACE(HAL, "received %u bytes", packet_size);
    return static_cast<ssize_t>(copy_len);
}

int WindowsDevice::get_fd() const
{
    // Windows 不使用 fd, 返回 -1
    return -1;
}

std::string WindowsDevice::get_name() const
{
    return _name;
}

#endif // NEUSTACK_PLATFORM_WINDOWS
