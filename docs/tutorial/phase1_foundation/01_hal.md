# HAL (Hardware Abstraction Layer) 实现教程

## 1. 设计目标

HAL 层的目标是**屏蔽不同操作系统的虚拟网卡接口差异**，向上层提供统一的收发包接口。

```
┌─────────────────────────────────────────┐
│           上层协议 (IP/ICMP/TCP)         │
│              统一调用接口                 │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│          NetDevice (抽象基类)            │
│   open() / close() / send() / recv()    │
└─────────────────┬───────────────────────┘
                  │
        ┌─────────┼─────────┐
        ▼         ▼         ▼
   ┌────────┐ ┌────────┐ ┌────────┐
   │ macOS  │ │ Linux  │ │Windows │
   │  utun  │ │  TAP   │ │ Wintun │
   └────────┘ └────────┘ └────────┘
```

---

## 2. 抽象基类设计

### 2.1 头文件位置

```
include/neustack/hal/device.hpp
```

### 2.2 接口设计要点

你的抽象基类需要包含以下接口：

| 方法 | 返回类型 | 说明 |
|------|----------|------|
| `open()` | `int` | 创建并打开虚拟网卡，成功返回 0 |
| `close()` | `int` | 关闭设备，释放资源 |
| `send(data, len)` | `ssize_t` | 发送数据，返回实际发送字节数 |
| `recv(buf, len, timeout_ms)` | `ssize_t` | 接收数据，支持超时，返回实际接收字节数 |
| `get_fd()` | `int` | 获取文件描述符 (用于 select/poll) |
| `get_name()` | `string` | 获取设备名 (如 "utun3") |
| `set_ip(ip, netmask)` | `int` | 配置 IP 地址 (可选，也可用外部脚本) |

### 2.3 设计建议

```cpp
// 伪代码示意，你需要自己实现
class NetDevice {
public:
    // 虚析构函数 (重要！)
    virtual ~NetDevice() = default;

    // 纯虚函数 - 子类必须实现
    virtual int open() = 0;
    virtual int close() = 0;
    virtual ssize_t send(const uint8_t* data, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len, int timeout_ms = -1) = 0;
    virtual int get_fd() const = 0;
    virtual std::string get_name() const = 0;

    // 工厂方法 - 根据平台创建具体实现
    static std::unique_ptr<NetDevice> create();

    // 禁止拷贝 (RAII 资源管理)
    NetDevice(const NetDevice&) = delete;
    NetDevice& operator=(const NetDevice&) = delete;

protected:
    NetDevice() = default;  // 只能通过子类或工厂创建
};
```

### 2.4 工厂方法实现思路

```cpp
// 在 src/hal/device.cpp 中实现
std::unique_ptr<NetDevice> NetDevice::create() {
#if defined(NEUSTACK_PLATFORM_MACOS)
    return std::make_unique<MacOSDevice>();
#elif defined(NEUSTACK_PLATFORM_LINUX)
    return std::make_unique<LinuxDevice>();
#elif defined(NEUSTACK_PLATFORM_WINDOWS)
    return std::make_unique<WindowsDevice>();
#else
    #error "Unsupported platform"
#endif
}
```

---

## 3. macOS 实现 (utun)

### 3.1 文件位置

```
src/hal/hal_macos.cpp
```

### 3.2 utun 设备概述

macOS 的 `utun` 是一个 **L3 (网络层) 点对点设备**：

- 不需要处理以太网帧，直接收发 IP 报文
- 不需要 ARP（点对点链路）
- 需要 root 权限创建

### 3.3 创建 utun 的步骤

#### Step 1: 创建控制 socket

```cpp
int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
if (fd < 0) {
    // 错误处理
}
```

**需要的头文件：**
```cpp
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/ioctl.h>
#include <net/if_utun.h>
#include <sys/sys_domain.h>
```

#### Step 2: 获取 utun 控制 ID

```cpp
struct ctl_info ctl_info;
memset(&ctl_info, 0, sizeof(ctl_info));
strncpy(ctl_info.ctl_name, UTUN_CONTROL_NAME, sizeof(ctl_info.ctl_name));

if (ioctl(fd, CTLIOCGINFO, &ctl_info) < 0) {
    // 错误处理
}
```

`UTUN_CONTROL_NAME` 是 `"com.apple.net.utun_control"`

#### Step 3: 连接到 utun

```cpp
struct sockaddr_ctl sc;
memset(&sc, 0, sizeof(sc));
sc.sc_len = sizeof(sc);
sc.sc_family = AF_SYSTEM;
sc.ss_sysaddr = AF_SYS_CONTROL;
sc.sc_id = ctl_info.ctl_id;
sc.sc_unit = 0;  // 0 = 让系统分配 utun 编号

if (connect(fd, (struct sockaddr*)&sc, sizeof(sc)) < 0) {
    // 错误处理
}
```

**关于 `sc_unit`：**
- `0` = 系统自动分配 (推荐)
- `N` = 请求 utun(N-1)，如 `sc_unit = 10` 请求 `utun9`

#### Step 4: 获取分配的设备名

```cpp
char ifname[IFNAMSIZ];
socklen_t ifname_len = sizeof(ifname);

if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &ifname_len) < 0) {
    // 错误处理
}
// ifname 现在是 "utun3" 之类的字符串
```

### 3.4 收发数据

#### 发送数据

**重要：macOS utun 发送时需要在数据前加 4 字节的协议族标识**

```cpp
// 发送 IP 报文
ssize_t send_packet(int fd, const uint8_t* ip_packet, size_t len) {
    // 构造带协议族前缀的缓冲区
    uint8_t buf[len + 4];

    // 4 字节协议族标识 (网络字节序)
    uint32_t proto = htonl(AF_INET);  // IPv4
    memcpy(buf, &proto, 4);
    memcpy(buf + 4, ip_packet, len);

    return write(fd, buf, len + 4);
}
```

#### 接收数据

```cpp
ssize_t recv_packet(int fd, uint8_t* buf, size_t buf_len) {
    uint8_t recv_buf[buf_len + 4];

    ssize_t n = read(fd, recv_buf, sizeof(recv_buf));
    if (n < 4) {
        return -1;  // 数据太短
    }

    // 跳过 4 字节协议族前缀
    memcpy(buf, recv_buf + 4, n - 4);
    return n - 4;
}
```

### 3.5 配置 IP 地址

创建 utun 后，需要配置 IP 地址才能使用。有两种方式：

#### 方式 A: 使用 shell 脚本 (推荐初期使用)

```bash
# 假设设备名是 utun3
sudo ifconfig utun3 10.0.0.1 10.0.0.2 up
sudo route add -net 10.0.0.0/24 -interface utun3
```

#### 方式 B: 使用 ioctl 编程配置

```cpp
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int set_ip_address(const char* ifname, const char* ip, const char* dst_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);

    // 设置本地 IP
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr->sin_addr);

    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        // 错误处理
    }

    // 设置目标 IP (点对点)
    inet_pton(AF_INET, dst_ip, &addr->sin_addr);
    if (ioctl(sock, SIOCSIFDSTADDR, &ifr) < 0) {
        // 错误处理
    }

    // 启用接口
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        // 错误处理
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        // 错误处理
    }

    close(sock);
    return 0;
}
```

### 3.6 完整流程

```
1. socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)
2. ioctl(fd, CTLIOCGINFO, &ctl_info)
3. connect(fd, &sockaddr_ctl, ...)
4. getsockopt(..., UTUN_OPT_IFNAME, ...) → 获取设备名
5. 配置 IP 地址 (ifconfig 或 ioctl)
6. 循环: read/write 收发数据
7. close(fd) 关闭设备
```

---

## 4. Linux 实现 (TAP/TUN)

### 4.1 文件位置

```
src/hal/hal_linux.cpp
```

### 4.2 TUN vs TAP

| 类型 | 层级 | 数据格式 | 是否需要 ARP |
|------|------|----------|--------------|
| TUN | L3 | IP 报文 | 否 |
| TAP | L2 | 以太网帧 | 是 |

**建议**：使用 TUN，与 macOS utun 行为一致，简化上层代码。

### 4.3 创建 TUN 设备的步骤

#### Step 1: 打开 /dev/net/tun

```cpp
int fd = open("/dev/net/tun", O_RDWR);
if (fd < 0) {
    // 可能需要: sudo modprobe tun
}
```

**需要的头文件：**
```cpp
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <fcntl.h>
```

#### Step 2: 配置设备

```cpp
struct ifreq ifr;
memset(&ifr, 0, sizeof(ifr));

ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN 设备，无协议信息前缀
strncpy(ifr.ifr_name, "tun0", IFNAMSIZ);  // 指定名称，或留空让系统分配

if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
    // 错误处理
}

// ifr.ifr_name 现在包含实际分配的设备名
```

**关于 `IFF_NO_PI`：**
- 设置此标志：直接收发 IP 报文（与 macOS utun 一致）
- 不设置：每个包前有 4 字节的 `struct tun_pi` 头

#### Step 3: 配置 IP 地址

```bash
sudo ip addr add 10.0.0.1/24 dev tun0
sudo ip link set tun0 up
```

或使用 ioctl（与 macOS 类似）

### 4.4 收发数据

由于设置了 `IFF_NO_PI`，收发与普通文件 I/O 一样：

```cpp
// 发送
ssize_t n = write(fd, ip_packet, len);

// 接收
ssize_t n = read(fd, buf, buf_len);
```

**注意：与 macOS utun 不同，Linux TUN (设置 IFF_NO_PI 后) 不需要协议族前缀！**

---

## 5. Windows 实现 (Wintun)

### 5.1 文件位置

```
src/hal/hal_windows.cpp
```

### 5.2 Wintun 概述

Wintun 是 WireGuard 团队开发的轻量级 TUN 驱动，比传统 TAP-Windows 更高效。

- 官网：https://www.wintun.net/
- 需要下载 `wintun.dll` 并放入项目目录

### 5.3 动态加载 Wintun

```cpp
#include <windows.h>
#include "wintun.h"  // 从 Wintun SDK 获取

// 函数指针类型
typedef WINTUN_CREATE_ADAPTER_FUNC *WINTUN_CREATE_ADAPTER;
typedef WINTUN_OPEN_ADAPTER_FUNC *WINTUN_OPEN_ADAPTER;
// ... 其他函数指针

// 加载 DLL
HMODULE wintun = LoadLibraryW(L"wintun.dll");
if (!wintun) {
    // 错误处理
}

// 获取函数指针
auto WintunCreateAdapter = (WINTUN_CREATE_ADAPTER)GetProcAddress(wintun, "WintunCreateAdapter");
// ... 获取其他函数指针
```

### 5.4 创建适配器

```cpp
GUID guid;  // 使用固定 GUID 或 CoCreateGuid() 生成
WINTUN_ADAPTER_HANDLE adapter = WintunCreateAdapter(L"NeuStack", L"NeuStack Tunnel", &guid);
if (!adapter) {
    // 错误处理
}
```

### 5.5 创建会话 (Ring Buffer)

Wintun 使用 Ring Buffer 进行高效收发：

```cpp
WINTUN_SESSION_HANDLE session = WintunStartSession(adapter, 0x400000);  // 4MB ring
if (!session) {
    // 错误处理
}
```

### 5.6 收发数据

```cpp
// 接收
BYTE* packet;
DWORD packet_size;
packet = WintunReceivePacket(session, &packet_size);
if (packet) {
    // 处理 packet
    WintunReleaseReceivePacket(session, packet);
}

// 发送
BYTE* send_buf = WintunAllocateSendPacket(session, packet_size);
if (send_buf) {
    memcpy(send_buf, ip_packet, packet_size);
    WintunSendPacket(session, send_buf);
}
```

### 5.7 配置 IP 地址

使用 Windows API 或 netsh：

```powershell
netsh interface ip set address "NeuStack" static 10.0.0.1 255.255.255.0
```

---

## 6. 统一接口的差异处理

### 6.1 协议族前缀问题

| 平台 | 收发时是否有前缀 |
|------|------------------|
| macOS utun | ✅ 4 字节协议族 (需处理) |
| Linux TUN (IFF_NO_PI) | ❌ 无前缀 |
| Windows Wintun | ❌ 无前缀 |

**建议**：在各平台实现内部处理前缀，确保上层看到的都是纯 IP 报文。

### 6.2 非阻塞 I/O 与超时

#### macOS / Linux

```cpp
// 设置非阻塞
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// 使用 poll/select 实现超时
struct pollfd pfd = { .fd = fd, .events = POLLIN };
int ret = poll(&pfd, 1, timeout_ms);
if (ret > 0 && (pfd.revents & POLLIN)) {
    read(fd, buf, len);
}
```

#### Windows

```cpp
// Wintun 提供等待事件
HANDLE event = WintunGetReadWaitEvent(session);
WaitForSingleObject(event, timeout_ms);
```

---

## 7. 测试 HAL

### 7.1 最小化测试

完成 HAL 后，写一个简单的测试程序：

```cpp
int main() {
    auto device = NetDevice::create();

    if (device->open() < 0) {
        std::cerr << "Failed to open device\n";
        return 1;
    }

    std::cout << "Device: " << device->get_name() << "\n";
    std::cout << "FD: " << device->get_fd() << "\n";

    // 此时手动配置 IP:
    // sudo ifconfig <device_name> 10.0.0.1 10.0.0.2 up

    uint8_t buf[2048];
    while (true) {
        ssize_t n = device->recv(buf, sizeof(buf), 5000);
        if (n > 0) {
            std::cout << "Received " << n << " bytes\n";
            // 打印 hex dump
        } else if (n == 0) {
            std::cout << "Timeout\n";
        } else {
            std::cerr << "Error\n";
            break;
        }
    }

    device->close();
    return 0;
}
```

### 7.2 验证方法

```bash
# 终端 1: 运行程序
sudo ./build/neustack

# 终端 2: 配置 IP 并 ping
sudo ifconfig utun3 10.0.0.1 10.0.0.2 up
ping 10.0.0.2  # 这会发送 ICMP 包到你的程序
```

如果程序输出 "Received XX bytes"，说明 HAL 工作正常。

---

## 8. 常见问题

### Q1: macOS 报错 "Operation not permitted"

需要 root 权限：`sudo ./neustack`

### Q2: Linux 报错 "No such file or directory" (/dev/net/tun)

加载 TUN 模块：`sudo modprobe tun`

### Q3: 如何指定 utun 编号？

设置 `sc_unit = N+1` 来请求 `utunN`。例如想要 `utun9`，设置 `sc_unit = 10`。

### Q4: 为什么收到的数据长度不对？

检查是否正确处理了 macOS 的 4 字节协议族前缀。

### Q5: poll/select 超时不工作？

确保设置了 `O_NONBLOCK`，否则 `read()` 会阻塞。

---

## 9. 参考资料

### macOS
- Apple 官方没有公开 utun 文档，但可参考：
  - https://newosxbook.com/bonus/vol1ch17.html
  - WireGuard macOS 源码

### Linux
- `Documentation/networking/tuntap.txt` (内核文档)
- `man 4 tun`

### Windows
- https://www.wintun.net/
- https://git.zx2c4.com/wintun/about/

---

## 10. 检查清单

完成 HAL 实现后，确保：

- [ ] `open()` 能成功创建设备
- [ ] `get_name()` 返回正确的设备名
- [ ] `get_fd()` 返回有效的文件描述符
- [ ] `recv()` 能收到 ping 包 (配置 IP 后)
- [ ] `recv()` 超时功能正常
- [ ] `send()` 能发送数据 (后续 ICMP 实现时验证)
- [ ] `close()` 正确释放资源
- [ ] 无内存泄漏 (使用 AddressSanitizer 检查)
