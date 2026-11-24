/*
 * sender - BLG337E Project 1
 *
 * Reliable file transfer over UDP: Stop-and-Wait, Go-Back-N, Selective Repeat.
 *
 * Usage:
 *   sender --file F --port P [--host 127.0.0.1] [--mode sw|gbn|sr]
 *          [--window W] [--k K] [--payload N] [--log PATH]
 *          [--rto-init MS] [--rto-min MS] [--rto-max MS] [--max-retx N] [--unsafe]
 *
 * Protocol conventions (the receiver must follow the same ones):
 *   - Sequence numbers are counted modulo M = 2^k. Internally the sender
 *     uses 64-bit absolute packet indices; only seq_mod(idx, k) is sent.
 *   - sw / gbn: ACK number = CUMULATIVE, i.e. the sequence number of the next
 *     packet the receiver expects (everything before it was received).
 *   - sr: ACK number = sequence number of the single packet being
 *     acknowledged (individual ACK).
 *   - After the last DATA packet the sender sends FIN with number
 *     seq_mod(total_packets, k) and retransmits it until FIN_ACK with the
 *     same number arrives.
 *   - Window limits: sw W=1, gbn W <= 2^k - 1, sr W <= 2^(k-1).
 *
 * RTO: RFC 6298 estimator (SRTT/RTTVAR, alpha=1/8, beta=1/4, K=4), Karn's
 * rule (retransmitted packets give no RTT sample), exponential backoff on
 * timeout that is kept until the next valid sample.
 *
 * Log (CSV, one event per line, t_us = absolute CLOCK_MONOTONIC microseconds,
 * so sender/receiver/emulator logs share one time axis):
 *   t_us,event,type,seq,abs,len,base,next,rto_us,rtt_us,note
 */
#include <errno.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "packet.h"

typedef enum { MODE_SW, MODE_GBN, MODE_SR } mode_t_;

typedef struct {
    const char *file, *host, *log_path;
    int         port;
    mode_t_     mode;
    unsigned    window, k, payload;
    int64_t     rto_init_us, rto_min_us, rto_max_us;
    unsigned    max_retx;
    int         unsafe;     /* allow W beyond the protocol limit (exp. 5) */
} opts_t;

enum { ST_UNSENT = 0, ST_SENT = 1, ST_ACKED = 2 };

typedef struct {
    uint8_t  state;
    uint32_t tx_count;   /* number of transmissions so far */
    int64_t  sent_us;    /* time of last transmission      */
} slot_t;

/* ---- globals kept simple: single-threaded tool ------------------------- */

static FILE   *g_log;
static int64_t g_t0;
static int     g_sock = -1;

/* RTO estimator state */
static int     g_have_rtt;
static double  g_srtt, g_rttvar;
static int64_t g_rto;
static const opts_t *g_o;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void logev(const char *event, const char *type, long seq, long long abs_idx,
                  long len, long long base, long long next, long long rtt,
                  const char *note)
{
    fprintf(g_log, "%lld,%s,%s,%ld,%lld,%ld,%lld,%lld,%lld,%lld,%s\n",
            (long long)now_us(), event, type, seq, abs_idx, len, base,
            next, (long long)g_rto, rtt, note ? note : "");
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[sender] error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---- RTO ---------------------------------------------------------------- */

static int64_t clamp_rto(int64_t v)
{
    if (v < g_o->rto_min_us) v = g_o->rto_min_us;
    if (v > g_o->rto_max_us) v = g_o->rto_max_us;
    return v;
}

static void rtt_sample(int64_t r)
{
    if (!g_have_rtt) {
        g_srtt = (double)r;
        g_rttvar = (double)r / 2.0;
        g_have_rtt = 1;
    } else {
        double err = g_srtt - (double)r;
        if (err < 0) err = -err;
        g_rttvar = 0.75 * g_rttvar + 0.25 * err;
        g_srtt = 0.875 * g_srtt + 0.125 * (double)r;
    }
    g_rto = clamp_rto((int64_t)(g_srtt + 4.0 * g_rttvar));
}

static void rto_backoff(void)
{
    g_rto = clamp_rto(g_rto * 2);
}

/* ---- packet I/O --------------------------------------------------------- */

static const char *type_name(unsigned t)
{
    switch (t) {
    case PKT_DATA:    return "DATA";
    case PKT_ACK:     return "ACK";
    case PKT_FIN:     return "FIN";
    case PKT_FIN_ACK: return "FIN_ACK";
    default:          return "?";
    }
}

static void send_pkt(uint8_t type, uint32_t seq, const uint8_t *pl, uint16_t len)
{
    uint8_t buf[MAX_PACKET_SIZE];
    size_t n = pkt_encode(buf, type, seq, pl, len);
    if (send(g_sock, buf, n, 0) < 0 && errno != ECONNREFUSED && errno != EAGAIN)
        die("send: %s", strerror(errno));
}

/* Receive one datagram if available. Returns 1 if a valid packet was parsed,
 * 0 if nothing (or an invalid packet, which is logged and dropped). */
static int recv_pkt(packet_header_t *h, long long base, long long next)
{
    uint8_t buf[MAX_PACKET_SIZE + 64];
    ssize_t n = recv(g_sock, buf, sizeof buf, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED ||
            errno == EINTR)
            return -1;
        die("recv: %s", strerror(errno));
    }
    const uint8_t *pl;
    pkt_status_t st = pkt_decode(buf, (size_t)n, h, &pl);
    if (st != PKT_OK) {
        logev("RECV_BAD", "?", -1, -1, (long)n, base, next, -1,
              st == PKT_ERR_CRC ? "crc" : "format");
        return 0;
    }
    return 1;
}

/* ---- main --------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: sender --file F --port P [--host H] [--mode sw|gbn|sr]\n"
        "              [--window W] [--k K] [--payload N] [--log PATH]\n"
        "              [--rto-init MS] [--rto-min MS] [--rto-max MS] [--max-retx N]\n"
        "defaults: host 127.0.0.1, mode sr, W 16, k 7, payload 1024,\n"
        "          log logs/sender.log, rto-init 1000, rto-min 200, rto-max 8000,\n"
        "          max-retx 30\n");
    exit(2);
}

static long parse_num(const char *s, const char *name, long lo, long hi)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        die("invalid value for --%s: %s (expected %ld..%ld)", name, s, lo, hi);
    return v;
}

static void parse_args(int argc, char **argv, opts_t *o)
{
    *o = (opts_t){ .host = "127.0.0.1", .log_path = "logs/sender.log", .port = -1,
                   .mode = MODE_SR, .window = 16, .k = 7, .payload = 1024,
                   .rto_init_us = 1000000, .rto_min_us = 200000,
                   .rto_max_us = 8000000, .max_retx = 30 };
    static const struct option longopts[] = {
        {"file", 1, 0, 'f'}, {"host", 1, 0, 'h'}, {"port", 1, 0, 'p'},
        {"mode", 1, 0, 'm'}, {"window", 1, 0, 'w'}, {"k", 1, 0, 'k'},
        {"payload", 1, 0, 's'}, {"log", 1, 0, 'l'}, {"rto-init", 1, 0, 'i'},
        {"rto-min", 1, 0, 'n'}, {"rto-max", 1, 0, 'x'}, {"max-retx", 1, 0, 'r'}, {"unsafe", 0, 0, 'u'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 'f': o->file = optarg; break;
        case 'h': o->host = optarg; break;
        case 'p': o->port = (int)parse_num(optarg, "port", 1, 65535); break;
        case 'm':
            if (!strcmp(optarg, "sw")) o->mode = MODE_SW;
            else if (!strcmp(optarg, "gbn")) o->mode = MODE_GBN;
            else if (!strcmp(optarg, "sr")) o->mode = MODE_SR;
            else die("--mode must be sw, gbn or sr");
            break;
        case 'w': o->window = (unsigned)parse_num(optarg, "window", 1, 1 << 8); break;
        case 'k': o->k = (unsigned)parse_num(optarg, "k", SEQ_BITS_MIN, SEQ_BITS_MAX); break;
        case 's': o->payload = (unsigned)parse_num(optarg, "payload", 1, MAX_PAYLOAD_SIZE); break;
        case 'l': o->log_path = optarg; break;
        case 'i': o->rto_init_us = parse_num(optarg, "rto-init", 1, 600000) * 1000; break;
        case 'n': o->rto_min_us = parse_num(optarg, "rto-min", 1, 600000) * 1000; break;
        case 'x': o->rto_max_us = parse_num(optarg, "rto-max", 1, 600000) * 1000; break;
        case 'r': o->max_retx = (unsigned)parse_num(optarg, "max-retx", 1, 1000000); break;
        case 'u': o->unsafe = 1; break;
        default: usage();
        }
    }
    if (!o->file || o->port < 0)
        usage();

    unsigned m = 1u << o->k;
    if (o->mode == MODE_SW) o->window = 1;
    if (o->unsafe) {
        if (o->window > m) die("even with --unsafe, W must be <= 2^k (= %u)", m);
    } else if (o->mode == MODE_GBN && o->window > m - 1) {
        die("go-back-n needs W <= 2^k - 1 (= %u); use --unsafe to override", m - 1);
    } else if (o->mode == MODE_SR && o->window > m / 2) {
        die("selective-repeat needs W <= 2^(k-1) (= %u); use --unsafe to override", m / 2);
    }
    if (o->rto_min_us > o->rto_max_us)
        die("--rto-min must not exceed --rto-max");
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) die("out of memory");
    size_t r;
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) die("out of memory");
        }
    }
    if (ferror(f)) die("read error on %s", path);
    fclose(f);
    *len = n;
    return buf;
}

/* Transmit (or retransmit) data packet with absolute index idx. */
static void tx_data(const opts_t *o, const uint8_t *data, size_t fsize, slot_t *s,
                    long long idx, long long base, long long next, const char *why)
{
    size_t off = (size_t)idx * o->payload;
    size_t len = fsize - off < o->payload ? fsize - off : o->payload;
    uint32_t seq = seq_mod((uint32_t)idx, o->k);
    send_pkt(PKT_DATA, seq, data + off, (uint16_t)len);
    s->tx_count++;
    s->sent_us = now_us();
    s->state = ST_SENT;
    logev(s->tx_count == 1 ? "SEND" : "RESEND", "DATA", seq, idx, (long)len, base,
          next, -1, why);
}

int main(int argc, char *argv[])
{
    opts_t o;
    parse_args(argc, argv, &o);
    g_o = &o;
    g_rto = clamp_rto(o.rto_init_us);

    size_t fsize;
    uint8_t *data = read_file(o.file, &fsize);
    long long npkts = (long long)((fsize + o.payload - 1) / o.payload);
    const unsigned M = 1u << o.k;

    g_log = fopen(o.log_path, "w");
    if (!g_log) die("cannot open log %s: %s", o.log_path, strerror(errno));
    fprintf(g_log, "t_us,event,type,seq,abs,len,base,next,rto_us,rtt_us,note\n");

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) die("socket: %s", strerror(errno));
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons((uint16_t)o.port) };
    if (inet_pton(AF_INET, o.host, &peer.sin_addr) != 1) die("bad --host %s", o.host);
    if (connect(g_sock, (struct sockaddr *)&peer, sizeof peer) < 0)
        die("connect: %s", strerror(errno));

    slot_t *slots = calloc((size_t)npkts + 1, sizeof *slots);
    if (!slots) die("out of memory");

    g_t0 = now_us();
    logev("START", "-", -1, -1, (long)fsize, 0, 0, -1,
          o.mode == MODE_SW ? "sw" : o.mode == MODE_GBN ? "gbn" : "sr");

    long long base = 0, next = 0;
    int64_t gbn_timer = 0;          /* GBN/SW: start of the single timer */
    long long retx_total = 0;

    /* ---------------- DATA phase ---------------- */
    while (base < npkts) {
        /* fill the window */
        while (next < npkts && next - base < (long long)o.window) {
            if (o.mode != MODE_SR && base == next)
                gbn_timer = now_us();
            tx_data(&o, data, fsize, &slots[next], next, base, next + 1, "");
            next++;
        }

        /* earliest deadline */
        int64_t deadline;
        if (o.mode == MODE_SR) {
            deadline = INT64_MAX;
            for (long long i = base; i < next; i++)
                if (slots[i].state == ST_SENT && slots[i].sent_us + g_rto < deadline)
                    deadline = slots[i].sent_us + g_rto;
        } else {
            deadline = gbn_timer + g_rto;
        }
        int64_t wait = deadline - now_us();
        int ms = wait <= 0 ? 0 : (int)((wait + 999) / 1000);

        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        int pr = poll(&pfd, 1, ms);
        if (pr < 0 && errno != EINTR) die("poll: %s", strerror(errno));

        /* drain incoming ACKs */
        if (pr > 0) {
            packet_header_t h;
            int r;
            while ((r = recv_pkt(&h, base, next)) >= 0) {
                if (r == 0) continue;
                if (h.type != PKT_ACK || h.number >= M) {
                    logev("RECV_IGN", type_name(h.type), h.number, -1, 0, base, next,
                          -1, "unexpected");
                    continue;
                }
                int64_t t = now_us();
                uint32_t delta = seq_mod(h.number - (uint32_t)base, o.k);
                long long inflight = next - base;

                if (o.mode != MODE_SR) {
                    /* cumulative ACK */
                    if (delta == 0) {
                        logev("RECV_ACK", "ACK", h.number, base, 0, base, next, -1, "dup");
                    } else if ((long long)delta <= inflight) {
                        long long last = base + delta - 1;
                        long long rtt = -1;
                        if (slots[last].tx_count == 1) {
                            rtt = t - slots[last].sent_us;
                            rtt_sample(rtt);
                        }
                        for (long long i = base; i <= last; i++) slots[i].state = ST_ACKED;
                        base += delta;
                        gbn_timer = t;
                        logev("RECV_ACK", "ACK", h.number, last, 0, base, next, rtt, "new");
                    } else {
                        logev("RECV_ACK", "ACK", h.number, -1, 0, base, next, -1, "stale");
                    }
                } else {
                    /* individual ACK */
                    if ((long long)delta < inflight) {
                        long long idx = base + delta;
                        if (slots[idx].state == ST_SENT) {
                            long long rtt = -1;
                            if (slots[idx].tx_count == 1) {
                                rtt = t - slots[idx].sent_us;
                                rtt_sample(rtt);
                            }
                            slots[idx].state = ST_ACKED;
                            while (base < next && slots[base].state == ST_ACKED) base++;
                            logev("RECV_ACK", "ACK", h.number, idx, 0, base, next, rtt, "new");
                        } else {
                            logev("RECV_ACK", "ACK", h.number, idx, 0, base, next, -1, "dup");
                        }
                    } else {
                        logev("RECV_ACK", "ACK", h.number, -1, 0, base, next, -1, "stale");
                    }
                }
            }
        }

        /* timeouts */
        int64_t t = now_us();
        if (base >= npkts) break;
        if (o.mode == MODE_SR) {
            int fired = 0;
            for (long long i = base; i < next; i++) {
                if (slots[i].state != ST_SENT || t < slots[i].sent_us + g_rto) continue;
                if (slots[i].tx_count > o.max_retx)
                    die("packet %lld not acknowledged after %u retransmissions", i, o.max_retx);
                if (!fired) {
                    logev("TIMEOUT", "DATA", seq_mod((uint32_t)i, o.k), i, 0, base, next, -1, "");
                    rto_backoff();
                    fired = 1;
                }
                tx_data(&o, data, fsize, &slots[i], i, base, next, "timeout");
                retx_total++;
            }
        } else if (t >= gbn_timer + g_rto && base < next) {
            if (slots[base].tx_count > o.max_retx)
                die("packet %lld not acknowledged after %u retransmissions", base, o.max_retx);
            logev("TIMEOUT", "DATA", seq_mod((uint32_t)base, o.k), base, 0, base, next, -1, "");
            rto_backoff();
            gbn_timer = t;
            for (long long i = base; i < next; i++) {
                tx_data(&o, data, fsize, &slots[i], i, base, next, "timeout");
                retx_total++;
            }
        }
    }

    /* ---------------- FIN phase ---------------- */
    uint32_t fin_seq = seq_mod((uint32_t)npkts, o.k);
    unsigned fin_tx = 0;
    int done = 0;
    while (!done) {
        if (fin_tx > o.max_retx) die("FIN not acknowledged after %u retransmissions", o.max_retx);
        send_pkt(PKT_FIN, fin_seq, NULL, 0);
        fin_tx++;
        logev(fin_tx == 1 ? "SEND" : "RESEND", "FIN", fin_seq, npkts, 0, base, next, -1, "");
        int64_t until = now_us() + g_rto;
        for (;;) {
            int64_t wait = until - now_us();
            if (wait <= 0) break;
            struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
            int pr = poll(&pfd, 1, (int)((wait + 999) / 1000));
            if (pr < 0 && errno != EINTR) die("poll: %s", strerror(errno));
            if (pr <= 0) continue;
            packet_header_t h;
            int r;
            while ((r = recv_pkt(&h, base, next)) >= 0) {
                if (r == 0) continue;
                if (h.type == PKT_FIN_ACK && h.number == fin_seq) {
                    logev("RECV_ACK", "FIN_ACK", h.number, npkts, 0, base, next, -1, "");
                    done = 1;
                    break;
                }
                logev("RECV_IGN", type_name(h.type), h.number, -1, 0, base, next, -1, "late");
            }
            if (done) break;
        }
        if (!done) {
            logev("TIMEOUT", "FIN", fin_seq, npkts, 0, base, next, -1, "");
            rto_backoff();
        }
    }

    double secs = (double)(now_us() - g_t0) / 1e6;
    logev("DONE", "-", -1, -1, (long)fsize, base, next, -1, "");
    fclose(g_log);
    close(g_sock);
    fprintf(stderr,
            "[sender] done: %zu bytes, %lld packets, %lld retransmissions, "
            "%.3f s, %.1f kbit/s goodput, final RTO %.1f ms\n",
            fsize, npkts, retx_total, secs,
            secs > 0 ? (double)fsize * 8.0 / secs / 1000.0 : 0.0,
            (double)g_rto / 1000.0);
    free(slots);
    free(data);
    return 0;
}
