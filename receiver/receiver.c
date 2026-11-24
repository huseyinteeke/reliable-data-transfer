/*
 * receiver - BLG337E Project 1
 *
 * Usage:
 *   receiver --port P --out FILE [--mode sw|gbn|sr] [--window W] [--k K]
 *            [--log PATH] [--linger MS]
 *
 * Replies go to the source address of each datagram (the emulator).
 *
 *   sw / gbn: accepts only the next expected packet; every DATA packet is
 *             answered with a CUMULATIVE ACK = seq of the next expected packet.
 *   sr:       accepts packets in [expected, expected+W) (buffering out-of-order
 *             ones), delivers in order, answers each with an individual ACK
 *             (the packet's own seq). Packets in the previous window
 *             [expected-W, expected) are re-acknowledged (their ACK was lost).
 *   FIN:      if its number equals the expected seq (all data delivered) the
 *             file is closed and FIN_ACK is sent. The receiver then lingers so
 *             it can answer retransmitted FINs whose FIN_ACK was lost.
 *
 * Log (CSV, t_us = absolute CLOCK_MONOTONIC microseconds):
 *   t_us,event,type,seq,abs,len,expected,note
 */
#include <errno.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "packet.h"

typedef enum { MODE_SW, MODE_GBN, MODE_SR } rmode_t;

static FILE *g_log, *g_out;
static int g_sock = -1;
static volatile sig_atomic_t g_stop;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void logev(const char *ev, const char *type, long seq, long long abs_idx,
                  long len, long long expected, const char *note)
{
    fprintf(g_log, "%lld,%s,%s,%ld,%lld,%ld,%lld,%s\n", (long long)now_us(), ev,
            type, seq, abs_idx, len, expected, note ? note : "");
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[receiver] error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static long parse_num(const char *s, const char *name, long lo, long hi)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        die("invalid value for --%s: %s (expected %ld..%ld)", name, s, lo, hi);
    return v;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: receiver --port P --out FILE [--mode sw|gbn|sr] [--window W]\n"
        "                [--k K] [--log PATH] [--linger MS]\n"
        "defaults: mode sr, W 16, k 7, log logs/receiver.log, linger 2000\n");
    exit(2);
}

static void send_to(const struct sockaddr_in *to, uint8_t type, uint32_t num)
{
    uint8_t buf[HEADER_SIZE];
    size_t n = pkt_encode(buf, type, num, NULL, 0);
    if (sendto(g_sock, buf, n, 0, (const struct sockaddr *)to, sizeof *to) < 0 &&
        errno != ECONNREFUSED)
        die("sendto: %s", strerror(errno));
}

int main(int argc, char *argv[])
{
    const char *out_path = NULL, *log_path = "logs/receiver.log";
    int port = -1;
    rmode_t mode = MODE_SR;
    unsigned window = 16, k = 7;
    long linger_ms = 2000;

    static const struct option longopts[] = {
        {"port", 1, 0, 'p'}, {"out", 1, 0, 'o'}, {"mode", 1, 0, 'm'},
        {"window", 1, 0, 'w'}, {"k", 1, 0, 'k'}, {"log", 1, 0, 'l'},
        {"linger", 1, 0, 'g'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 'p': port = (int)parse_num(optarg, "port", 1, 65535); break;
        case 'o': out_path = optarg; break;
        case 'm':
            if (!strcmp(optarg, "sw")) mode = MODE_SW;
            else if (!strcmp(optarg, "gbn")) mode = MODE_GBN;
            else if (!strcmp(optarg, "sr")) mode = MODE_SR;
            else die("--mode must be sw, gbn or sr");
            break;
        case 'w': window = (unsigned)parse_num(optarg, "window", 1, 256); break;
        case 'k': k = (unsigned)parse_num(optarg, "k", SEQ_BITS_MIN, SEQ_BITS_MAX); break;
        case 'l': log_path = optarg; break;
        case 'g': linger_ms = parse_num(optarg, "linger", 0, 600000); break;
        default: usage();
        }
    }
    if (port < 0 || !out_path) usage();
    const unsigned M = 1u << k;
    if (window > M) die("W must be <= 2^k (= %u)", M);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_log = fopen(log_path, "w");
    if (!g_log) die("cannot open log %s: %s", log_path, strerror(errno));
    fprintf(g_log, "t_us,event,type,seq,abs,len,expected,note\n");
    g_out = fopen(out_path, "wb");
    if (!g_out) die("cannot open %s: %s", out_path, strerror(errno));

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) die("socket: %s", strerror(errno));
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(g_sock, (struct sockaddr *)&me, sizeof me) < 0)
        die("bind: %s", strerror(errno));

    /* SR reorder buffer, indexed by seq */
    uint8_t (*rbuf)[MAX_PAYLOAD_SIZE] = malloc((size_t)M * MAX_PAYLOAD_SIZE);
    uint16_t *rlen = calloc(M, sizeof *rlen);
    uint8_t *have = calloc(M, 1);
    if (!rbuf || !rlen || !have) die("out of memory");

    long long expected = 0;        /* absolute index of next packet to deliver */
    long long bytes = 0, dups = 0;
    int finished = 0;
    logev("START", "-", -1, -1, 0, 0,
          mode == MODE_SW ? "sw" : mode == MODE_GBN ? "gbn" : "sr");

    while (!g_stop) {
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        int pr = poll(&pfd, 1, finished ? (int)linger_ms : -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            die("poll: %s", strerror(errno));
        }
        if (pr == 0) break; /* lingered long enough after FIN_ACK */

        uint8_t buf[MAX_PACKET_SIZE + 64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR || errno == ECONNREFUSED) continue;
            die("recvfrom: %s", strerror(errno));
        }
        packet_header_t h;
        const uint8_t *pl;
        pkt_status_t st = pkt_decode(buf, (size_t)n, &h, &pl);
        if (st != PKT_OK) {
            logev("RECV_BAD", "?", -1, -1, (long)n, expected,
                  st == PKT_ERR_CRC ? "crc" : "format");
            continue; /* corrupted: silently dropped, sender's timer recovers */
        }
        if (h.number >= M) {
            logev("RECV_BAD", "?", h.number, -1, h.payload_len, expected, "range");
            continue;
        }

        if (h.type == PKT_DATA) {
            uint32_t exp_seq = seq_mod((uint32_t)expected, k);
            if (mode != MODE_SR) {
                if (h.number == exp_seq) {
                    fwrite(pl, 1, h.payload_len, g_out);
                    fflush(g_out);
                    bytes += h.payload_len;
                    logev("RECV", "DATA", h.number, expected, h.payload_len, expected, "new");
                    expected++;
                } else {
                    dups++;
                    logev("RECV", "DATA", h.number, -1, h.payload_len, expected, "unexpected");
                }
                uint32_t a = seq_mod((uint32_t)expected, k);
                send_to(&from, PKT_ACK, a);
                logev("SEND", "ACK", a, -1, 0, expected, "cumulative");
            } else {
                uint32_t d = seq_mod(h.number - exp_seq, k);
                if (d < window) {
                    long long idx = expected + d;
                    if (!have[h.number]) {
                        memcpy(rbuf[h.number], pl, h.payload_len);
                        rlen[h.number] = h.payload_len;
                        have[h.number] = 1;
                        logev("RECV", "DATA", h.number, idx, h.payload_len, expected,
                              d == 0 ? "new" : "buffered");
                    } else {
                        dups++;
                        logev("RECV", "DATA", h.number, idx, h.payload_len, expected, "dup");
                    }
                    while (have[seq_mod((uint32_t)expected, k)]) {
                        uint32_t s = seq_mod((uint32_t)expected, k);
                        fwrite(rbuf[s], 1, rlen[s], g_out);
                        bytes += rlen[s];
                        have[s] = 0;
                        expected++;
                    }
                    fflush(g_out);
                    send_to(&from, PKT_ACK, h.number);
                    logev("SEND", "ACK", h.number, idx, 0, expected, "individual");
                } else if (d >= M - window) {
                    dups++;
                    logev("RECV", "DATA", h.number, -1, h.payload_len, expected, "old");
                    send_to(&from, PKT_ACK, h.number);
                    logev("SEND", "ACK", h.number, -1, 0, expected, "re-ack");
                } else {
                    logev("RECV", "DATA", h.number, -1, h.payload_len, expected, "outside");
                }
            }
        } else if (h.type == PKT_FIN) {
            if (h.number == seq_mod((uint32_t)expected, k)) {
                logev("RECV", "FIN", h.number, expected, 0, expected,
                      finished ? "dup" : "ok");
                fflush(g_out);
                send_to(&from, PKT_FIN_ACK, h.number);
                logev("SEND", "FIN_ACK", h.number, expected, 0, expected, "");
                finished = 1;
            } else {
                logev("RECV", "FIN", h.number, -1, 0, expected, "early");
            }
        } else {
            logev("RECV_IGN", "?", h.number, -1, h.payload_len, expected, "type");
        }
    }

    logev("DONE", "-", -1, -1, (long)bytes, expected, finished ? "complete" : "incomplete");
    fclose(g_out);
    fclose(g_log);
    close(g_sock);
    fprintf(stderr, "[receiver] %s: %lld bytes in %lld packets, %lld duplicates\n",
            finished ? "complete" : "INCOMPLETE", bytes, expected, dups);
    free(rbuf); free(rlen); free(have);
    return finished ? 0 : 1;
}
