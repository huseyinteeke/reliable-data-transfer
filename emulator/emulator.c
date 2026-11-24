/*
 * emulator - BLG337E Project 1: UDP impairment emulator
 *
 * Usage:
 *   emulator --listen-port P --dst-port Q --scenario FILE [--seed N]
 *            [--dst-host 127.0.0.1] [--log PATH] [--idle-exit SEC]
 *
 *   sender --> [listen-port] EMULATOR [ephemeral port] --> receiver (dst-port)
 *   Packets from the sender arrive on the listen socket (direction "fwd");
 *   packets from the receiver arrive on the second socket (direction "rev")
 *   and are returned to the sender's address learned from its first packet.
 *
 * Per-direction model (each direction has its own rate, queue, delay):
 *   1. arrival; per-packet actions and random draws (loss, corrupt, dup, reorder)
 *   2. loss: fwd drops DATA with p=data_loss, rev drops ACK with p=ack_loss
 *      (FIN / FIN_ACK are never randomly lost so that runs terminate)
 *   3. queue: if queue_*_pkts packets are already waiting/transmitting the
 *      packet is tail-dropped; otherwise it is serialized FIFO, taking
 *      8*len/rate seconds (len = UDP payload size).
 *   4. delivery time = transmission end + delay + U[-jitter,+jitter], clamped
 *      so jitter never changes delivery order.
 *   5. corruption flips one random bit of the datagram; duplication delivers
 *      a second copy right after the original; reordering holds a packet and
 *      delivers it right after the next packet of the same direction
 *      (released automatically after 1 s if none follows).
 *
 * Scenario file: "key = value" lines, '#' starts a comment.
 *   rate_fwd_bps rate_rev_bps queue_fwd_pkts queue_rev_pkts delay_fwd_ms
 *   delay_rev_ms jitter_ms data_loss ack_loss corrupt duplicate reorder seed
 *   action <drop|corrupt|duplicate> <data|ack> <number>
 *   action delay <data|ack> <number> <ms>
 * An action applies once, to the first packet of that kind whose header
 * "number" field equals <number> (numbers are the on-wire values, mod 2^k).
 * rate 0 means unlimited. The PRNG (splitmix64/xorshift) is seeded with
 * seed (CLI --seed overrides the file), so one scenario+seed gives one
 * sequence of random decisions.
 *
 * Log (CSV, t_us = absolute CLOCK_MONOTONIC microseconds):
 *   t_us,event,dir,type,number,len,note
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

#define BUFSZ (MAX_PACKET_SIZE + 64)
#define MAX_ACTIONS 256
#define HOLD_CAP_US 1000000

typedef struct {
    double rate[2], delay_ms[2], jitter_ms;
    long   queue[2];
    double data_loss, ack_loss, corrupt, dup, reorder;
    unsigned long long seed;
} scenario_t;

typedef enum { ACT_DROP, ACT_CORRUPT, ACT_DUP, ACT_DELAY } act_kind_t;
typedef struct {
    act_kind_t kind;
    int        dir;       /* 0 = data (fwd), 1 = ack (rev) */
    uint32_t   number;
    double     delay_ms;
    int        used;
} action_t;

typedef struct {
    int64_t  deliver_us;
    uint64_t order;
    uint64_t id;
    int      dir;
    size_t   len;
    uint8_t  buf[BUFSZ];
} pend_t;

typedef struct {
    int64_t link_free, last_deliver;
    int64_t *txq;               /* FIFO ring of transmission-end times */
    size_t  qhead, qcount, qcap;
    uint64_t held_id;           /* 0 = none */
} dirstate_t;

static scenario_t g_sc;
static action_t g_act[MAX_ACTIONS];
static int g_nact;
static FILE *g_log;
static volatile sig_atomic_t g_stop;
static uint64_t g_rng;

static pend_t *g_pend;
static size_t g_npend, g_pcap = 8192;
static uint64_t g_order, g_idgen;
static dirstate_t g_dir[2];

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[emulator] error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void on_signal(int s) { (void)s; g_stop = 1; }

/* ---- PRNG: splitmix64 seeding + xorshift64* ---------------------------- */

static void rng_seed(unsigned long long seed)
{
    unsigned long long z = seed + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    g_rng = z ^ (z >> 31);
    if (!g_rng) g_rng = 1;
}

static uint64_t rng_next(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static double rng_unit(void) { return (double)(rng_next() >> 11) / 9007199254740992.0; }

/* ---- logging ------------------------------------------------------------ */

static const char *type_name(unsigned t)
{
    switch (t) {
    case PKT_DATA: return "DATA";
    case PKT_ACK: return "ACK";
    case PKT_FIN: return "FIN";
    case PKT_FIN_ACK: return "FIN_ACK";
    default: return "?";
    }
}

static void logev(const char *ev, int dir, const uint8_t *buf, size_t len, const char *note)
{
    unsigned type = len > 0 ? buf[0] : 0;
    uint32_t num = 0;
    if (len >= HEADER_SIZE) {
        uint32_t n;
        memcpy(&n, buf + 4, 4);
        num = ntohl(n);
    }
    fprintf(g_log, "%lld,%s,%s,%s,%u,%zu,%s\n", (long long)now_us(), ev,
            dir == 0 ? "fwd" : "rev", type_name(type), num, len, note ? note : "");
}

/* ---- scenario parsing --------------------------------------------------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

static void load_scenario(const char *path)
{
    g_sc = (scenario_t){ .rate = {1e6, 1e6}, .queue = {128, 128},
                         .delay_ms = {20, 20}, .seed = 1 };
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open scenario %s: %s", path, strerror(errno));
    char line[512];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *s = trim(line);
        if (!*s) continue;

        if (!strncmp(s, "action", 6) && (s[6] == ' ' || s[6] == '\t')) {
            char kind[16], what[16];
            unsigned long num;
            double ms = 0;
            int got = sscanf(s + 6, "%15s %15s %lu %lf", kind, what, &num, &ms);
            if (got < 3) die("%s:%d: bad action", path, ln);
            if (g_nact >= MAX_ACTIONS) die("too many actions");
            action_t *a = &g_act[g_nact++];
            memset(a, 0, sizeof *a);
            if (!strcmp(kind, "drop")) a->kind = ACT_DROP;
            else if (!strcmp(kind, "corrupt")) a->kind = ACT_CORRUPT;
            else if (!strcmp(kind, "duplicate")) a->kind = ACT_DUP;
            else if (!strcmp(kind, "delay")) {
                if (got < 4) die("%s:%d: delay needs <ms>", path, ln);
                a->kind = ACT_DELAY;
                a->delay_ms = ms;
            } else die("%s:%d: unknown action %s", path, ln, kind);
            if (!strcmp(what, "data")) a->dir = 0;
            else if (!strcmp(what, "ack")) a->dir = 1;
            else die("%s:%d: action target must be data or ack", path, ln);
            a->number = (uint32_t)num;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) die("%s:%d: expected key = value", path, ln);
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        double v = atof(val);
        if (!strcmp(key, "rate_fwd_bps")) g_sc.rate[0] = v;
        else if (!strcmp(key, "rate_rev_bps")) g_sc.rate[1] = v;
        else if (!strcmp(key, "queue_fwd_pkts")) g_sc.queue[0] = (long)v;
        else if (!strcmp(key, "queue_rev_pkts")) g_sc.queue[1] = (long)v;
        else if (!strcmp(key, "delay_fwd_ms")) g_sc.delay_ms[0] = v;
        else if (!strcmp(key, "delay_rev_ms")) g_sc.delay_ms[1] = v;
        else if (!strcmp(key, "jitter_ms")) g_sc.jitter_ms = v;
        else if (!strcmp(key, "data_loss")) g_sc.data_loss = v;
        else if (!strcmp(key, "ack_loss")) g_sc.ack_loss = v;
        else if (!strcmp(key, "corrupt")) g_sc.corrupt = v;
        else if (!strcmp(key, "duplicate")) g_sc.dup = v;
        else if (!strcmp(key, "reorder")) g_sc.reorder = v;
        else if (!strcmp(key, "seed")) g_sc.seed = strtoull(val, NULL, 10);
        else die("%s:%d: unknown key %s", path, ln, key);
    }
    fclose(f);
    if (g_sc.queue[0] < 1 || g_sc.queue[1] < 1) die("queue capacity must be >= 1");
}

/* ---- pending (in-flight) set ------------------------------------------- */

static pend_t *pend_add(int dir, const uint8_t *buf, size_t len, int64_t deliver)
{
    if (g_npend == g_pcap) return NULL;
    pend_t *p = &g_pend[g_npend++];
    p->deliver_us = deliver;
    p->order = g_order++;
    p->id = ++g_idgen;
    p->dir = dir;
    p->len = len;
    memcpy(p->buf, buf, len);
    return p;
}

static void txq_push(dirstate_t *d, int64_t end)
{
    if (d->qcount == d->qcap) {
        size_t ncap = d->qcap ? d->qcap * 2 : 256;
        int64_t *n = malloc(ncap * sizeof *n);
        if (!n) die("out of memory");
        for (size_t i = 0; i < d->qcount; i++) n[i] = d->txq[(d->qhead + i) % d->qcap];
        free(d->txq);
        d->txq = n; d->qcap = ncap; d->qhead = 0;
    }
    d->txq[(d->qhead + d->qcount++) % d->qcap] = end;
}

/* ---- arrival handling --------------------------------------------------- */

static action_t *find_action(int dir, uint32_t num, act_kind_t kind)
{
    for (int i = 0; i < g_nact; i++)
        if (!g_act[i].used && g_act[i].dir == dir && g_act[i].number == num &&
            g_act[i].kind == kind)
            return &g_act[i];
    return NULL;
}

static void arrive(int dir, const uint8_t *in, size_t len)
{
    int64_t now = now_us();
    dirstate_t *d = &g_dir[dir];
    uint8_t buf[BUFSZ];
    memcpy(buf, in, len);

    unsigned type = len > 0 ? buf[0] : 0;
    uint32_t num = 0;
    if (len >= HEADER_SIZE) { uint32_t n; memcpy(&n, buf + 4, 4); num = ntohl(n); }
    int is_loss_type = dir == 0 ? type == PKT_DATA : type == PKT_ACK;
    logev("ARRIVE", dir, buf, len, "");

    /* fixed number of random draws per packet keeps runs reproducible */
    double r_loss = rng_unit(), r_cor = rng_unit(), r_dup = rng_unit(), r_re = rng_unit();
    double r_jit = rng_unit();
    uint64_t r_bit = rng_next();

    action_t *a_drop = find_action(dir, num, ACT_DROP);
    action_t *a_cor = find_action(dir, num, ACT_CORRUPT);
    action_t *a_dup = find_action(dir, num, ACT_DUP);
    action_t *a_del = find_action(dir, num, ACT_DELAY);
    if (type != PKT_DATA && type != PKT_ACK) a_drop = a_cor = a_dup = a_del = NULL;

    double ploss = dir == 0 ? g_sc.data_loss : g_sc.ack_loss;
    if (a_drop || (is_loss_type && r_loss < ploss)) {
        if (a_drop) a_drop->used = 1;
        logev("DROP", dir, buf, len, a_drop ? "action" : "loss");
        return;
    }

    /* queue admission */
    while (d->qcount > 0 && d->txq[d->qhead] <= now) {
        d->qhead = (d->qhead + 1) % d->qcap;
        d->qcount--;
    }
    if (d->qcount >= (size_t)g_sc.queue[dir]) {
        logev("DROP", dir, buf, len, "queue");
        return;
    }
    int64_t start = d->link_free > now ? d->link_free : now;
    int64_t txtime = g_sc.rate[dir] > 0 ? (int64_t)((double)len * 8.0 * 1e6 / g_sc.rate[dir]) : 0;
    int64_t end = start + txtime;
    d->link_free = end;
    txq_push(d, end);

    int64_t jit = (int64_t)((r_jit * 2.0 - 1.0) * g_sc.jitter_ms * 1000.0);
    int64_t deliver = end + (int64_t)(g_sc.delay_ms[dir] * 1000.0) + jit;
    if (deliver < d->last_deliver) deliver = d->last_deliver;
    d->last_deliver = deliver;

    if (a_del) {
        a_del->used = 1;
        deliver += (int64_t)(a_del->delay_ms * 1000.0);
        logev("DELAY", dir, buf, len, "action");
    }
    if (a_cor || r_cor < g_sc.corrupt) {
        if (a_cor) a_cor->used = 1;
        size_t bit = (size_t)(r_bit % (uint64_t)(len * 8));
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        logev("CORRUPT", dir, buf, len, a_cor ? "action" : "random");
    }

    int hold = d->held_id == 0 && r_re < g_sc.reorder;
    pend_t *p = pend_add(dir, buf, len, hold ? deliver + HOLD_CAP_US : deliver);
    if (!p) { logev("DROP", dir, buf, len, "overflow"); return; }
    uint64_t pid = p->id;
    if (hold) {
        d->held_id = pid;
        logev("HOLD", dir, buf, len, "reorder");
        return;
    }
    if (a_dup || r_dup < g_sc.dup) {
        if (a_dup) a_dup->used = 1;
        pend_add(dir, buf, len, deliver);
        logev("DUPLICATE", dir, buf, len, a_dup ? "action" : "random");
    }
    /* release a held packet right after this one (and its duplicate) */
    if (d->held_id) {
        for (size_t i = 0; i < g_npend; i++) {
            if (g_pend[i].id == d->held_id) {
                g_pend[i].deliver_us = deliver;
                g_pend[i].order = g_order++;
                break;
            }
        }
        d->held_id = 0;
    }
}

static void deliver_due(int sock_s, int sock_r, const struct sockaddr_in *rx,
                        const struct sockaddr_in *tx, int have_tx)
{
    for (;;) {
        int64_t now = now_us();
        size_t best = g_npend;
        for (size_t i = 0; i < g_npend; i++) {
            if (g_pend[i].deliver_us > now) continue;
            if (best == g_npend || g_pend[i].deliver_us < g_pend[best].deliver_us ||
                (g_pend[i].deliver_us == g_pend[best].deliver_us &&
                 g_pend[i].order < g_pend[best].order))
                best = i;
        }
        if (best == g_npend) return;
        pend_t p = g_pend[best];
        g_pend[best] = g_pend[--g_npend];
        if (g_dir[p.dir].held_id == p.id) g_dir[p.dir].held_id = 0;

        ssize_t r;
        if (p.dir == 0)
            r = sendto(sock_r, p.buf, p.len, 0, (const struct sockaddr *)rx, sizeof *rx);
        else if (have_tx)
            r = sendto(sock_s, p.buf, p.len, 0, (const struct sockaddr *)tx, sizeof *tx);
        else
            r = 0;
        if (r < 0 && errno != ECONNREFUSED) die("sendto: %s", strerror(errno));
        logev("DELIVER", p.dir, p.buf, p.len, "");
    }
}

int main(int argc, char *argv[])
{
    const char *scen = NULL, *log_path = "logs/emulator.log", *dst_host = "127.0.0.1";
    int lport = -1, dport = -1, idle_exit = 0;
    long long seed_cli = -1;
    static const struct option lo[] = {
        {"listen-port", 1, 0, 'l'}, {"dst-port", 1, 0, 'd'}, {"dst-host", 1, 0, 'h'},
        {"scenario", 1, 0, 's'}, {"seed", 1, 0, 'e'}, {"log", 1, 0, 'g'},
        {"idle-exit", 1, 0, 'i'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 'l': lport = atoi(optarg); break;
        case 'd': dport = atoi(optarg); break;
        case 'h': dst_host = optarg; break;
        case 's': scen = optarg; break;
        case 'e': seed_cli = atoll(optarg); break;
        case 'g': log_path = optarg; break;
        case 'i': idle_exit = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: emulator --listen-port P --dst-port Q --scenario FILE "
                            "[--seed N] [--dst-host H] [--log PATH] [--idle-exit SEC]\n");
            return 2;
        }
    }
    if (lport < 1 || dport < 1 || !scen) die("--listen-port, --dst-port and --scenario are required");
    load_scenario(scen);
    if (seed_cli >= 0) g_sc.seed = (unsigned long long)seed_cli;
    rng_seed(g_sc.seed);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_log = fopen(log_path, "w");
    if (!g_log) die("cannot open log %s: %s", log_path, strerror(errno));
    fprintf(g_log, "t_us,event,dir,type,number,len,note\n");
    g_pend = malloc(g_pcap * sizeof *g_pend);
    if (!g_pend) die("out of memory");

    int ss = socket(AF_INET, SOCK_DGRAM, 0), sr = socket(AF_INET, SOCK_DGRAM, 0);
    if (ss < 0 || sr < 0) die("socket: %s", strerror(errno));
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)lport),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(ss, (struct sockaddr *)&me, sizeof me) < 0) die("bind: %s", strerror(errno));
    struct sockaddr_in rx = { .sin_family = AF_INET, .sin_port = htons((uint16_t)dport) };
    if (inet_pton(AF_INET, dst_host, &rx.sin_addr) != 1) die("bad --dst-host");

    struct sockaddr_in tx;
    int have_tx = 0;
    int64_t last_act = now_us();
    fprintf(g_log, "%lld,START,-,-,0,0,seed=%llu\n", (long long)now_us(),
            g_sc.seed);

    while (!g_stop) {
        int64_t now = now_us(), wake = INT64_MAX;
        for (size_t i = 0; i < g_npend; i++)
            if (g_pend[i].deliver_us < wake) wake = g_pend[i].deliver_us;
        int ms = wake == INT64_MAX ? 200 : (wake <= now ? 0 : (int)((wake - now + 999) / 1000));
        if (ms > 200) ms = 200;
        struct pollfd pf[2] = { { ss, POLLIN, 0 }, { sr, POLLIN, 0 } };
        int pr = poll(pf, 2, ms);
        if (pr < 0 && errno != EINTR) die("poll: %s", strerror(errno));

        for (int i = 0; i < 2 && pr > 0; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            for (;;) {
                uint8_t buf[BUFSZ];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                ssize_t n = recvfrom(i == 0 ? ss : sr, buf, sizeof buf, MSG_DONTWAIT,
                                     (struct sockaddr *)&from, &fl);
                if (n < 0) break;
                last_act = now_us();
                if (i == 0) { tx = from; have_tx = 1; }
                arrive(i, buf, (size_t)n);
            }
        }
        deliver_due(ss, sr, &rx, &tx, have_tx);
        if (idle_exit > 0 && g_npend == 0 && now_us() - last_act > (int64_t)idle_exit * 1000000)
            break;
    }
    fprintf(g_log, "%lld,DONE,-,-,0,0,\n", (long long)now_us());
    fclose(g_log);
    return 0;
}
