/**
 * udp_flood — 高速 UDP 发包工具
 *
 * 使用 sendmmsg() 批量发送，替代 Python 的 socket.sendto()。
 * 配合 run_throughput_test.sh 使用。
 *
 * Usage: ./udp_flood <target_ip> <port> <payload_size> <duration_s>
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static constexpr int BATCH = 64;
static constexpr int MAX_PAYLOAD = 9000;

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <ip> <port> <payload_size> <duration_s>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = std::atoi(argv[2]);
    int payload_size = std::atoi(argv[3]);
    int duration = std::atoi(argv[4]);

    if (payload_size > MAX_PAYLOAD) payload_size = MAX_PAYLOAD;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip, &dst.sin_addr);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst)) < 0) {
        perror("connect");
        return 1;
    }

    char payload[MAX_PAYLOAD];
    std::memset(payload, 0xAA, payload_size);

    struct iovec iov[BATCH];
    struct mmsghdr msgs[BATCH];
    std::memset(msgs, 0, sizeof(msgs));

    for (int i = 0; i < BATCH; i++) {
        iov[i].iov_base = payload;
        iov[i].iov_len = static_cast<size_t>(payload_size);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct timespec start{}, now{};
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned long long total = 0;

    for (;;) {
        int sent = sendmmsg(fd, msgs, BATCH, MSG_DONTWAIT);
        if (sent > 0) total += static_cast<unsigned long long>(sent);

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec)
                       + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= duration) break;
    }

    close(fd);

    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - start.tv_sec)
                   + (now.tv_nsec - start.tv_nsec) / 1e9;

    fprintf(stderr, "Sent %llu packets in %.1fs (%.3f Mpps)\n",
            total, elapsed, total / elapsed / 1e6);
    return 0;
}
