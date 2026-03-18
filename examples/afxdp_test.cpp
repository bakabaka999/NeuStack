// examples/afxdp_test.cpp
//
// AF_XDP 数据通路验证 — 通过 NeuStack 门面类使用
//
// 用法:
//   sudo ./build/examples/afxdp_test [--iface eth0] [--ip 10.0.0.2]
//
// 功能:
//   1. 以 AF_XDP 模式启动协议栈
//   2. 自动响应 ICMP ping
//   3. Ctrl+C 退出

#include "neustack/neustack.hpp"
#include "neustack/common/log.hpp"
#include <cstdio>
#include <cstring>

using namespace neustack;

static void print_usage(const char* prog) {
    printf("AF_XDP data-path test via NeuStack facade\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --iface NAME   Network interface (default: eth0)\n");
    printf("  --ip ADDR      Local IP address (default: 192.168.100.2)\n");
    printf("  -v             Verbose (DEBUG log level)\n");
    printf("  -h, --help     Show this help\n");
    printf("\nRequires root. The stack auto-detects MAC addresses.\n");
    printf("SSH traffic (port 22) is always passed to the kernel.\n");
}

int main(int argc, char* argv[]) {
    StackConfig cfg;
    cfg.device_type = "af_xdp";
    cfg.enable_firewall = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc)
            cfg.device_ifname = argv[++i];
        else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc)
            cfg.local_ip = argv[++i];
        else if (strcmp(argv[i], "-v") == 0)
            cfg.log_level = LogLevel::DEBUG;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("=== NeuStack AF_XDP Test ===\n");
    printf("  Interface: %s\n",
           cfg.device_ifname.empty() ? "eth0 (default)" : cfg.device_ifname.c_str());
    printf("  Local IP:  %s\n", cfg.local_ip.c_str());
    printf("  Mode:      AF_XDP\n\n");

    auto stack = NeuStack::create(cfg);
    if (!stack) {
        fprintf(stderr, "ERROR: Failed to create NeuStack (need root? interface exists?)\n");
        return 1;
    }
    printf("Stack initialized, ICMP ping should work.\n");
    printf("  Try: ping %s\n", cfg.local_ip.c_str());
    printf("  Press Ctrl+C to stop.\n\n");

    stack->run();

    printf("\nDone.\n");
    return 0;
}
