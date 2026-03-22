#ifdef NEUSTACK_PLATFORM_LINUX

#include "neustack/hal/umem.hpp"

#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/if_xdp.h>
#include <cstring>
#include <cerrno>

using namespace neustack;

Umem::Umem() : Umem(Config{}) {}

Umem::Umem(const Config &config)
    : _config(config)
    , _allocator(config.frame_count, config.frame_size)
{}

Umem::~Umem() {
    if (_buffer) {
        munmap(_buffer, _buffer_size);
        _buffer = nullptr;
    }
}

int Umem::create() {
    _buffer_size = static_cast<size_t>(_config.frame_count) * _config.frame_size;

    // 对齐分配的内存
    _buffer = static_cast<uint8_t *>(mmap(
        nullptr,                        // 映射起始地址由内核选择
        _buffer_size,                   // 映射的字节大小
        PROT_READ | PROT_WRITE,         // 保护标志
        MAP_PRIVATE | MAP_ANONYMOUS,    // 映射类型
        -1, 0                           // 文件描述符与映射偏移量
    ));

    if (_buffer == MAP_FAILED) {
        _buffer = nullptr;
        return -1;
    }

    // 请求透明大页，减少 TLB miss
    madvise(_buffer, _buffer_size, MADV_HUGEPAGE);

    // 锁定内存，防止运行时 page fault
    mlock(_buffer, _buffer_size);

    // 预热页面（防止首次访问的缺页中断）
    memset(_buffer, 0, _buffer_size);

    return 0;
}

int Umem::register_to_socket(int xsk_fd) {
    // 创建reg结构体
    struct xdp_umem_reg umem_reg = {}; // 来自 Linux C语言库
    umem_reg.addr = reinterpret_cast<uint64_t>(_buffer);
    umem_reg.len = _buffer_size;
    umem_reg.chunk_size = _config.frame_size;
    umem_reg.headroom = _config.headroom;
    umem_reg.flags = _config.flags;

    // 利用结构体注册到socket中
    int ret = setsockopt(xsk_fd, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg));
    if (ret < 0) {
        return -1;
    }

    return 0;
}

#endif // NEUSTACK_PLATFORM_LINUX