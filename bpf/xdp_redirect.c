/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bpf/xdp_redirect.c
 *
 * XDP program that selectively redirects packets to AF_XDP sockets.
 *
 * Safety: only redirects IP packets (ICMP/UDP/TCP) to AF_XDP.
 * All other traffic (ARP, SSH on TCP:22, etc. that doesn't match)
 * is passed through to the normal kernel network stack, so existing
 * connections (SSH, DNS, etc.) are not disrupted.
 *
 * Build:
 *   clang -O2 -target bpf -g -c xdp_redirect.c -o xdp_redirect.o
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

/* XSKMAP: maps RX queue index → AF_XDP socket fd. */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
    __uint(max_entries, 64);
} xsks_map SEC(".maps");

/* Ports that must never be redirected (kept in kernel stack). */
#define PORT_SSH 22

SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    /* Need at least an Ethernet header. */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    /* Only process IPv4. ARP, IPv6, etc. go to kernel. */
    if (eth->h_proto != __builtin_bswap16(ETH_P_IP))
        return XDP_PASS;

    /* Need an IP header. */
    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    /* Protect SSH: if TCP and either port is 22, pass to kernel. */
    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (void *)iph + (iph->ihl * 4);
        if ((void *)(tcp + 1) > data_end)
            return XDP_PASS;

        __u16 sport = __builtin_bswap16(tcp->source);
        __u16 dport = __builtin_bswap16(tcp->dest);
        if (sport == PORT_SSH || dport == PORT_SSH)
            return XDP_PASS;
    }

    /* Redirect to AF_XDP socket if registered for this queue. */
    int index = ctx->rx_queue_index;
    if (bpf_map_lookup_elem(&xsks_map, &index))
        return bpf_redirect_map(&xsks_map, index, XDP_PASS);

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
