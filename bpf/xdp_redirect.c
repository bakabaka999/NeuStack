/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bpf/xdp_redirect.c
 *
 * Minimal XDP program that redirects packets to AF_XDP sockets.
 *
 * - Looks up the RX queue index in xsks_map (BPF_MAP_TYPE_XSKMAP).
 * - If a socket is registered for this queue, redirect the packet to it.
 * - Otherwise, pass the packet up to the normal kernel network stack.
 *
 * Build:
 *   clang -O2 -target bpf -g -c xdp_redirect.c -o xdp_redirect.o
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* XSKMAP: maps RX queue index → AF_XDP socket fd. */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
    __uint(max_entries, 64);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx)
{
    int index = ctx->rx_queue_index;

    /* If an AF_XDP socket is registered for this queue, redirect. */
    if (bpf_map_lookup_elem(&xsks_map, &index))
        return bpf_redirect_map(&xsks_map, index, XDP_PASS);

    /* No socket for this queue — pass to kernel stack. */
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
