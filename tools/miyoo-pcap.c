/* Minimal receive-only monitor capture for the stock Miyoo RTL8188FU driver.
 * GPL-3.0-or-later. No libpcap dependency. Mode changes use Wireless Extensions
 * because this driver does not advertise monitor mode through nl80211.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/wireless.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
static double monotime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void put16(FILE *f, uint16_t v)
{
    fputc(v & 255, f); fputc(v >> 8, f);
}
static void put32(FILE *f, uint32_t v)
{
    put16(f, v & 65535); put16(f, v >> 16);
}
int main(int argc, char **argv)
{
    int s, type;
    struct iwreq wr;
    struct ifreq ir;
    const char *iface;
    if (argc < 4 || strlen(argv[1]) >= IFNAMSIZ) {
        fprintf(stderr, "usage: %s IFACE mode monitor|managed\n"
                        "       %s IFACE capture SECONDS OUTPUT.pcap\n", argv[0], argv[0]);
        return 2;
    }
    iface = argv[1];
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("control socket"); return 1; }
    memset(&wr, 0, sizeof(wr));
    strcpy(wr.ifr_name, iface);
    if (!strcmp(argv[2], "mode")) {
        if (!strcmp(argv[3], "monitor")) wr.u.mode = IW_MODE_MONITOR;
        else if (!strcmp(argv[3], "managed")) wr.u.mode = IW_MODE_INFRA;
        else { fprintf(stderr, "unknown mode\n"); close(s); return 2; }
        if (ioctl(s, SIOCSIWMODE, &wr) < 0) { perror("SIOCSIWMODE"); close(s); return 1; }
        close(s);
        return 0;
    }
    if (strcmp(argv[2], "capture") || argc != 5) { close(s); return 2; }
    char *end;
    long seconds = strtol(argv[3], &end, 10);
    if (*end || seconds < 1 || seconds > 300) {
        fprintf(stderr, "capture duration must be 1..300 seconds\n"); close(s); return 2;
    }
    memset(&ir, 0, sizeof(ir));
    strcpy(ir.ifr_name, iface);
    if (ioctl(s, SIOCGIFHWADDR, &ir) < 0) { perror("SIOCGIFHWADDR"); close(s); return 1; }
    type = ir.ifr_hwaddr.sa_family;
    if (type != 803) {
        fprintf(stderr, "refusing capture: interface type %d, expected radiotap (803)\n", type);
        close(s); return 1;
    }
    if (ioctl(s, SIOCGIFINDEX, &ir) < 0) { perror("SIOCGIFINDEX"); close(s); return 1; }
    int index = ir.ifr_ifindex;
    close(s);
    s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (s < 0) { perror("packet socket"); return 1; }
    struct sockaddr_ll bindaddr = {0};
    bindaddr.sll_family = AF_PACKET;
    bindaddr.sll_protocol = htons(ETH_P_ALL);
    bindaddr.sll_ifindex = index;
    if (bind(s, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
        perror("bind"); close(s); return 1;
    }
    int one = 1, rcvbuf = 256 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) < 0) {
        perror("SO_TIMESTAMPNS"); close(s); return 1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    FILE *out = fopen(argv[4], "wb");
    if (!out) { perror(argv[4]); close(s); return 1; }
    /* Classic little-endian PCAP, microsecond timestamps, LINKTYPE_RADIOTAP. */
    put32(out, 0xa1b2c3d4); put16(out, 2); put16(out, 4);
    put32(out, 0); put32(out, 0); put32(out, 65535); put32(out, 127);
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGHUP, stop);
    double deadline = monotime() + seconds;
    unsigned long packets = 0;
    int failed = 0;
    while (!stopped && monotime() < deadline) {
        fd_set fds;
        struct timeval wait = {0, 100000};
        FD_ZERO(&fds); FD_SET(s, &fds);
        int ready = select(s + 1, &fds, NULL, NULL, &wait);
        if (ready < 0) { if (errno == EINTR) continue; perror("select"); failed = 1; break; }
        if (!ready) continue;
        unsigned char buf[65535];
        union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(struct timespec))]; } control;
        struct sockaddr_ll from;
        struct iovec iov = {buf, sizeof(buf)};
        struct msghdr msg = {0};
        msg.msg_name = &from; msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
        ssize_t n = recvmsg(s, &msg, MSG_TRUNC);
        if (n < 0) { if (errno == EINTR) continue; perror("recvmsg"); failed = 1; break; }
        if (from.sll_pkttype == PACKET_OUTGOING) continue;
        struct timespec stamp = {0};
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS &&
                c->cmsg_len >= CMSG_LEN(sizeof(stamp))) memcpy(&stamp, CMSG_DATA(c), sizeof(stamp));
        }
        if (!stamp.tv_sec) { fprintf(stderr, "missing kernel timestamp\n"); failed = 1; break; }
        size_t saved = (size_t)n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        put32(out, stamp.tv_sec); put32(out, stamp.tv_nsec / 1000);
        put32(out, saved); put32(out, n);
        if (fwrite(buf, 1, saved, out) != saved) { perror("write capture"); failed = 1; break; }
        packets++;
    }
    struct tpacket_stats stats;
    socklen_t statslen = sizeof(stats);
    if (getsockopt(s, SOL_PACKET, PACKET_STATISTICS, &stats, &statslen) == 0)
        fprintf(stderr, "socket packets=%u dropped=%u\n", stats.tp_packets, stats.tp_drops);
    if (fclose(out)) { perror("close capture"); failed = 1; }
    close(s);
    fprintf(stderr, "saved %lu received frames to %s\n", packets, argv[4]);
    return failed ? 1 : 0;
}
