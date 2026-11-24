#!/usr/bin/env python3
"""Computes the metrics of PDF section 3.3 from the run logs and draws the plots.

Reads   logs/exp*/<run>/{meta.json,sender.log,receiver.log,emulator.log}
Writes  results/metrics.csv   one row per run (all metrics)
        results/summary.txt   aggregated tables (mean over seeds)
        plot/*.png

Metric definitions (exactly as computed here):
  completion_s   time of the first DATA transmission (sender log, event SEND)
                 until the last "new" ACK that acknowledges the last DATA
                 packet (sender log, RECV_ACK with note=new).
  goodput_bps    file_size*8 / completion_s
  retx_overhead  retransmitted DATA bytes / total transmitted DATA bytes
                 (bytes = payload + 12-byte header, counted at the sender:
                 events RESEND vs SEND+RESEND of type DATA)
  utilization    (bytes of first-time DATA transmissions * 8 / link rate)
                 / completion_s, i.e. the fraction of the forward link spent
                 sending new data (retransmissions excluded)
  integrity      SHA-256 of the received file equals that of the input
  RTT / RTO      rtt_us column of RECV_ACK events (only non-retransmitted
                 packets, Karn) and the rto_us column of every sender event
Theory (see the formulas in theory_*()):  t_d = (payload+12)*8/R,
  t_a = 12*8/R, prop = delay_fwd + delay_rev, C = t_d + t_a + prop,
  N = ceil(file_size / payload).
"""
import argparse, csv, json, math, statistics as st
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
HDR = 12
COL = {"gbn": "#d1495b", "sr": "#00798c", "sw": "#edae49"}
LABEL = {"gbn": "Go-Back-N", "sr": "Selective Repeat", "sw": "Stop-and-Wait"}


# --------------------------------------------------------------------------
# log parsing
# --------------------------------------------------------------------------
def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def analyse_run(d, meta):
    rows = read_csv(d / "sender.log")
    sc = meta["scenario"]
    rate = float(sc["rate_fwd_bps"])
    size = meta["file_size"]
    m = dict(exp=meta["exp"], name=meta["name"], tag=meta["tag"] or "", mode=meta["mode"],
             W=meta["W"], k=meta["k"], seed=meta["seed"], integrity=int(meta["match"]),
             sender_rc=meta["sender_rc"], delay_ms=sc["delay_fwd_ms"], jitter_ms=sc["jitter_ms"],
             data_loss=sc["data_loss"], ack_loss=sc["ack_loss"], corrupt=sc["corrupt"],
             duplicate=sc["duplicate"], reorder=sc["reorder"], file_size=size)
    sends = [r for r in rows if r["type"] == "DATA" and r["event"] in ("SEND", "RESEND")]
    new_b = sum(int(r["len"]) + HDR for r in sends if r["event"] == "SEND")
    re_b = sum(int(r["len"]) + HDR for r in sends if r["event"] == "RESEND")
    acks = [r for r in rows if r["event"] == "RECV_ACK" and r["type"] == "ACK" and r["note"] == "new"]
    done = bool(acks) and meta["sender_rc"] == 0
    if sends and done:
        t0, t1 = int(sends[0]["t_us"]), int(acks[-1]["t_us"])
        comp = (t1 - t0) / 1e6
    else:
        t0 = int(sends[0]["t_us"]) if sends else 0
        comp = float("nan")
    m.update(completion_s=comp,
             goodput_bps=size * 8 / comp if comp == comp and comp > 0 else float("nan"),
             retx_overhead=re_b / (new_b + re_b) if new_b + re_b else float("nan"),
             utilization=(new_b * 8 / rate) / comp if comp == comp and comp > 0 else float("nan"),
             tx_pkts=sum(1 for r in sends), retx_pkts=sum(1 for r in sends if r["event"] == "RESEND"),
             timeouts=sum(1 for r in rows if r["event"] == "TIMEOUT" and r["type"] == "DATA"),
             rtt_samples=sum(1 for r in rows if r["event"] == "RECV_ACK" and int(r["rtt_us"]) >= 0))
    rtts = [int(r["rtt_us"]) / 1000 for r in rows if r["event"] == "RECV_ACK" and int(r["rtt_us"]) >= 0]
    m["rtt_mean_ms"] = st.mean(rtts) if rtts else float("nan")
    rtos = [int(r["rto_us"]) / 1000 for r in rows if int(r["rto_us"]) > 0]
    m["rto_mean_ms"] = st.mean(rtos) if rtos else float("nan")
    to = [int(r["rto_us"]) / 1000 for r in rows if r["event"] == "TIMEOUT" and r["type"] == "DATA"]
    m["rto_at_timeout_ms"] = st.mean(to) if to else float("nan")

    # emulator + receiver counters
    em = defaultdict(int)
    for r in read_csv(d / "emulator.log"):
        if r["event"] in ("DROP", "CORRUPT", "DUPLICATE", "HOLD"):
            em[r["event"] + (":" + r["note"] if r["event"] == "DROP" else "") + "_" + r["dir"]] += 1
    m["emu_events"] = ";".join(f"{k}={v}" for k, v in sorted(em.items()))
    rx = read_csv(d / "receiver.log")
    m["rx_bad"] = sum(1 for r in rx if r["event"] == "RECV_BAD")
    m["rx_dup"] = sum(1 for r in rx if r["event"] == "RECV" and r["note"] in ("dup", "old", "unexpected"))
    # time series for the RTO plot
    series = dict(t0=t0,
                  rtt=[((int(r["t_us"]) - t0) / 1e6, int(r["rtt_us"]) / 1000)
                       for r in rows if r["event"] == "RECV_ACK" and int(r["rtt_us"]) >= 0],
                  rto=[((int(r["t_us"]) - t0) / 1e6, int(r["rto_us"]) / 1000)
                       for r in rows if r["event"] in ("RECV_ACK", "TIMEOUT") and int(r["rto_us"]) > 0])
    return m, series


# --------------------------------------------------------------------------
# theory
# --------------------------------------------------------------------------
def params(m, payload=1024):
    R = 1e6
    td = (payload + HDR) * 8 / R
    ta = HDR * 8 / R
    prop = (float(m["delay_ms"]) * 2) / 1000
    N = math.ceil(m["file_size"] / payload)
    return td, ta, prop, N, td + ta + prop


def theory_time_ideal(m, W):
    """Ideal network. A window of W packets is sent every cycle C until the
    pipe is full (W >= C/t_d); then the link is the bottleneck."""
    td, ta, prop, N, C = params(m)
    return max(math.ceil(N / W) * C, N * td + prop + ta)


def theory_util(m, W):
    td, ta, prop, N, C = params(m)
    return min(1.0, W * td / C)


def theory_overhead_loss(mode, p, W):
    """Fraction of transmitted bytes that are retransmissions.
    SR/SW: each packet is sent 1/(1-p) times on average -> p.
    GBN: a loss repeats ~W packets -> 1 - (1-p)/(1+(W-1)p)."""
    if mode == "gbn":
        return 1 - (1 - p) / (1 + (W - 1) * p)
    return p


def theory_time_loss(m, rto_s):
    """Completion time with data loss p; L = N p/(1-p) expected loss events,
    rto_s = RTO that expired at the timeouts (measured).
    SW : every loss costs one RTO instead of a round trip.
    GBN: after a loss everything sent until the timer fires is discarded, so
         the link is useless for one RTO per loss: T = N t_d + L*RTO.
    SR : only the lost packet is repeated (N t_d/(1-p) in total); the sender
         stalls only once the window is full: W t_d after the loss, until the
         RTO fires: L * max(0, RTO - W t_d)."""
    td, ta, prop, N, C = params(m)
    p, W, mode = float(m["data_loss"]), m["W"], m["mode"]
    L = N * p / (1 - p)
    if mode == "sw":
        return N * (C + p / (1 - p) * rto_s)
    if mode == "sr":
        return N * td / (1 - p) + L * max(0.0, rto_s - W * td) + prop + ta
    return N * td + L * rto_s + prop + ta


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def mean_of(rs, key):
    v = [r[key] for r in rs if r[key] == r[key]]
    return st.mean(v) if v else float("nan")


def group(rs, *keys):
    g = defaultdict(list)
    for r in rs:
        g[tuple(r[k] for k in keys)].append(r)
    return g


def save(fig, name):
    out = ROOT / "plot" / name
    out.parent.mkdir(exist_ok=True)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    plt.close(fig)
    print("wrote", out.relative_to(ROOT))


def style(ax, xlabel, ylabel, title):
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=10)
    ax.grid(alpha=0.3)


# --------------------------------------------------------------------------
# experiments
# --------------------------------------------------------------------------
def plot_exp1(rs, out):
    rs = [r for r in rs if r["exp"] == 1]
    if not rs:
        return
    sw = [r for r in rs if r["mode"] == "sw"]
    Ws = sorted({r["W"] for r in rs if r["mode"] != "sw"})
    base = rs[0]
    td, ta, prop, N, C = params(base)
    out.append(f"\n== Experiment 1: ideal network  (t_d={td*1e3:.3f} ms, t_a={ta*1e3:.3f} ms, "
               f"prop={prop*1e3:.0f} ms, C={C*1e3:.3f} ms, N={N}, W*=C/t_d={C/td:.2f})")
    out.append(f"{'proto':5} {'W':>3} {'T meas [s]':>11} {'T theory':>9} {'goodput kb/s':>13} "
               f"{'util meas':>10} {'util theory':>11} {'retx':>5} {'integrity':>9}")
    series = {}
    for mode in ("gbn", "sr"):
        series[mode] = {r["W"]: r for r in rs if r["mode"] == mode}
    for r in sw + [series[m][W] for m in ("gbn", "sr") for W in Ws if W in series[m]]:
        out.append(f"{r['mode']:5} {r['W']:>3} {r['completion_s']:11.3f} "
                   f"{theory_time_ideal(r, r['W']):9.3f} {r['goodput_bps']/1e3:13.1f} "
                   f"{r['utilization']:10.3f} {theory_util(r, r['W']):11.3f} "
                   f"{r['retx_pkts']:5d} {'OK' if r['integrity'] else 'FAIL':>9}")
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    for mode in ("gbn", "sr"):
        xs = [W for W in Ws if W in series[mode]]
        ax[0].plot(xs, [series[mode][W]["completion_s"] for W in xs], "o-", color=COL[mode], label=LABEL[mode])
        ax[1].plot(xs, [series[mode][W]["goodput_bps"] / 1e3 for W in xs], "o-", color=COL[mode], label=LABEL[mode])
        ax[2].plot(xs, [series[mode][W]["utilization"] for W in xs], "o-", color=COL[mode], label=LABEL[mode])
    th = [theory_time_ideal(base, W) for W in Ws]
    ax[0].plot(Ws, th, "k--", label="theory")
    ax[1].plot(Ws, [base["file_size"] * 8 / t / 1e3 for t in th], "k--", label="theory")
    ax[2].plot(Ws, [theory_util(base, W) for W in Ws], "k--", label="theory")
    if sw:
        ax[0].axhline(sw[0]["completion_s"], color=COL["sw"], ls=":", label="Stop-and-Wait")
        ax[1].axhline(sw[0]["goodput_bps"] / 1e3, color=COL["sw"], ls=":", label="Stop-and-Wait")
        ax[2].axhline(sw[0]["utilization"], color=COL["sw"], ls=":", label="Stop-and-Wait")
    for a, (yl, t) in zip(ax, [("completion time [s]", "Completion time"),
                               ("goodput [kbit/s]", "Goodput"), ("link utilization", "Link utilization")]):
        a.set_xscale("log", base=2)
        a.set_xticks(Ws)
        a.set_xticklabels(Ws)
        style(a, "window size W", yl, t)
        a.legend(fontsize=8)
    ax[1].axhline(1000, color="grey", lw=0.8)
    save(fig, "exp1_ideal.png")


def plot_exp2(rs, out):
    rs = [r for r in rs if r["exp"] == 2]
    if not rs:
        return
    g = group(rs, "mode", "data_loss")
    out.append("\n== Experiment 2: data packet loss (mean over seeds)")
    out.append(f"{'proto':5} {'loss':>5} {'n':>2} {'T [s]':>8} {'T theory':>9} {'goodput kb/s':>13} "
               f"{'retx ovh':>9} {'ovh theory':>10} {'timeouts':>8} {'integrity':>9}")
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    for mode in ("sw", "gbn", "sr"):
        ps = sorted(p for (m, p) in g if m == mode)
        T, G, O, TT, OT = [], [], [], [], []
        for p in ps:
            grp = g[(mode, p)]
            T.append(mean_of(grp, "completion_s"))
            G.append(mean_of(grp, "goodput_bps") / 1e3)
            O.append(mean_of(grp, "retx_overhead"))
            rto = mean_of(grp, "rto_at_timeout_ms") / 1e3
            rto = rto if rto == rto else 0.0
            TT.append(theory_time_loss(grp[0], rto))
            OT.append(theory_overhead_loss(mode, p, grp[0]["W"]))
            out.append(f"{mode:5} {p*100:4.0f}% {len(grp):2d} {T[-1]:8.2f} {TT[-1]:9.2f} {G[-1]:13.1f} "
                       f"{O[-1]:9.3f} {OT[-1]:10.3f} {mean_of(grp,'timeouts'):8.1f} "
                       f"{'OK' if all(x['integrity'] for x in grp) else 'FAIL':>9}")
        x = [p * 100 for p in ps]
        ax[0].plot(x, T, "o-", color=COL[mode], label=LABEL[mode])
        ax[0].plot(x, TT, "--", color=COL[mode], alpha=0.6, label=f"{LABEL[mode]} theory")
        ax[1].plot(x, G, "o-", color=COL[mode], label=LABEL[mode])
        ax[2].plot(x, [o * 100 for o in O], "o-", color=COL[mode], label=LABEL[mode])
        ax[2].plot(x, [o * 100 for o in OT], "--", color=COL[mode], alpha=0.6, label=f"{LABEL[mode]} theory")
    for a, (yl, t) in zip(ax, [("completion time [s]", "Completion time"),
                               ("goodput [kbit/s]", "Goodput"), ("retransmission overhead [%]", "Retransmission overhead")]):
        style(a, "data packet loss [%]", yl, t)
        a.set_xticks([0, 5, 10])
        a.legend(fontsize=7)
    ax[0].set_yscale("log")
    save(fig, "exp2_loss.png")


def plot_exp3(rs, series, out):
    rs = [r for r in rs if r["exp"] == 3]
    if not rs:
        return
    g = group(rs, "mode", "W", "delay_ms")
    out.append("\n== Experiment 3: propagation delay, jitter = 10 % (mean over seeds)")
    out.append(f"{'proto':5} {'W':>3} {'delay':>6} {'T [s]':>8} {'goodput kb/s':>13} "
               f"{'util meas':>10} {'util theory':>11} {'mean RTT':>9} {'mean RTO':>9} {'timeouts':>8} {'integrity':>9}")
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    combos = sorted({(m, W) for (m, W, d) in g}, key=lambda x: (x[0] != "sw", x[1]))
    for mode, W in combos:
        ds = sorted(d for (m, w, d) in g if (m, w) == (mode, W))
        U, UT, G, T = [], [], [], []
        for d in ds:
            grp = g[(mode, W, d)]
            U.append(mean_of(grp, "utilization"))
            UT.append(theory_util(grp[0], W))
            G.append(mean_of(grp, "goodput_bps") / 1e3)
            T.append(mean_of(grp, "completion_s"))
            out.append(f"{mode:5} {W:>3} {d:6.0f} {T[-1]:8.2f} {G[-1]:13.1f} {U[-1]:10.3f} {UT[-1]:11.3f} "
                       f"{mean_of(grp,'rtt_mean_ms'):9.1f} {mean_of(grp,'rto_mean_ms'):9.1f} "
                       f"{mean_of(grp,'timeouts'):8.1f} {'OK' if all(x['integrity'] for x in grp) else 'FAIL':>9}")
        lab = "Stop-and-Wait" if mode == "sw" else f"SR W={W}"
        c = COL["sw"] if mode == "sw" else ("#00798c" if W == 16 else "#30638e")
        ls = "-" if W != 64 else "-."
        ax[0].plot(ds, U, "o" + ls, color=c, label=lab)
        ax[0].plot(ds, UT, "--", color=c, alpha=0.6, label=lab + " theory")
        ax[1].plot(ds, G, "o" + ls, color=c, label=lab)
        ax[2].plot(ds, T, "o" + ls, color=c, label=lab)
    for a, (yl, t) in zip(ax, [("link utilization", "Link utilization"),
                               ("goodput [kbit/s]", "Goodput"), ("completion time [s]", "Completion time")]):
        style(a, "one-way propagation delay [ms]", yl, t)
        a.set_xticks([20, 50, 100, 200])
        a.legend(fontsize=7)
    ax[0].set_yscale("log")
    save(fig, "exp3_delay.png")

    # RTO tracking (first seed of each selected config)
    sel = [("sr", 16, 20), ("sr", 16, 200), ("sr", 64, 200), ("sw", 1, 200)]
    fig, axs = plt.subplots(2, 2, figsize=(13, 7))
    for a, (mode, W, d) in zip(axs.flat, sel):
        cand = sorted((r for r in rs if (r["mode"], r["W"], r["delay_ms"]) == (mode, W, d)),
                      key=lambda r: r["seed"])
        if not cand:
            continue
        s = series[(3, cand[0]["name"])]
        if s["rtt"]:
            a.plot(*zip(*s["rtt"]), ".", ms=3, color="#30638e", label="RTT samples")
        if s["rto"]:
            a.step(*zip(*s["rto"]), where="post", color="#d1495b", lw=1.2, label="RTO")
        a.axhline(2 * d, color="grey", ls=":", label="2 x propagation")
        style(a, "time since first packet [s]", "ms", f"{LABEL[mode]} W={W}, delay {d} ms, jitter {d*0.1:g} ms")
        a.legend(fontsize=8)
    save(fig, "exp3_rto_tracking.png")


def plot_exp4(rs, out):
    rs = [r for r in rs if r["exp"] == 4]
    if not rs:
        return
    imps = ["corrupt", "reorder", "duplicate", "ack_loss"]
    g = group(rs, "tag", "mode")
    out.append("\n== Experiment 4: 5 % corruption / reordering / duplication / ACK loss (mean over seeds)")
    out.append(f"{'impairment':10} {'proto':5} {'n':>2} {'T [s]':>8} {'goodput kb/s':>13} {'retx ovh':>9} "
               f"{'timeouts':>8} {'rx bad':>6} {'rx dup':>6} {'integrity':>9}")
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    w = 0.25
    for j, mode in enumerate(("sw", "gbn", "sr")):
        T, O, G = [], [], []
        for imp in imps:
            grp = g.get((imp, mode), [])
            if not grp:
                T.append(0); O.append(0); G.append(0)
                continue
            T.append(mean_of(grp, "completion_s"))
            O.append(mean_of(grp, "retx_overhead") * 100)
            G.append(mean_of(grp, "goodput_bps") / 1e3)
            out.append(f"{imp:10} {mode:5} {len(grp):2d} {T[-1]:8.2f} {G[-1]:13.1f} {O[-1]/100:9.3f} "
                       f"{mean_of(grp,'timeouts'):8.1f} {mean_of(grp,'rx_bad'):6.1f} {mean_of(grp,'rx_dup'):6.1f} "
                       f"{'OK' if all(x['integrity'] for x in grp) else 'FAIL':>9}")
        xs = [i + (j - 1) * w for i in range(len(imps))]
        for a, v in zip(ax, (T, G, O)):
            a.bar(xs, v, w, color=COL[mode], label=LABEL[mode])
    for a, yl in zip(ax, ("completion time [s]", "goodput [kbit/s]", "retransmission overhead [%]")):
        a.set_xticks(range(len(imps)))
        a.set_xticklabels(imps)
        style(a, "impairment (5 %)", yl, yl.split(" [")[0].capitalize())
        a.legend(fontsize=8)
    save(fig, "exp4_impairments.png")


def plot_exp5(rs, out):
    rs = [r for r in rs if r["exp"] == 5]
    if not rs:
        return
    out.append("\n== Experiment 5: sequence-number limits (SHA-256 of received file == input ?)")
    fig, axs = plt.subplots(1, 2, figsize=(13, 3.2))
    for a, k in zip(axs, (3, 4)):
        M = 1 << k
        for yi, mode in enumerate(("gbn", "sr")):
            lim = M - 1 if mode == "gbn" else M // 2
            row = sorted((r for r in rs if r["mode"] == mode and r["k"] == k), key=lambda r: r["W"])
            out.append(f"k={k} (M={M}) {mode.upper():3}: theoretical limit W<={lim}; "
                       + " ".join(f"W{r['W']}:{'ok' if r['integrity'] else 'FAIL'}" for r in row))
            for r in row:
                a.plot(r["W"], yi, "o" if r["integrity"] else "X", ms=11,
                       color="#2a9d8f" if r["integrity"] else "#d1495b")
            a.plot([lim + 0.5] * 2, [yi - 0.4, yi + 0.4], "k--")
        a.set_yticks([0, 1])
        a.set_yticklabels(["Go-Back-N", "Selective Repeat"])
        a.set_ylim(-0.6, 1.6)
        a.set_xticks(range(1, M + 1))
        style(a, "window size W", "", f"k={k}: M=2^k={M}; green = file intact, red = SHA-256 differs; "
              "dashed = theoretical limit", )
        a.title.set_fontsize(8)
    save(fig, "exp5_seqlimit.png")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default=str(ROOT / "logs"))
    a = ap.parse_args()
    metrics, series = [], {}
    for meta_path in sorted(Path(a.logs).glob("exp*/*/meta.json")):
        d = meta_path.parent
        meta = json.loads(meta_path.read_text())
        try:
            m, s = analyse_run(d, meta)
        except (FileNotFoundError, KeyError, ValueError) as e:
            print("skip", d, e)
            continue
        metrics.append(m)
        series[(m["exp"], m["name"])] = s
    if not metrics:
        raise SystemExit("no runs found; run scripts/run_experiments.py first")
    (ROOT / "results").mkdir(exist_ok=True)
    cols = [c for c in metrics[0]]
    with open(ROOT / "results" / "metrics.csv", "w", newline="") as f:
        w = csv.DictWriter(f, cols)
        w.writeheader()
        w.writerows(metrics)
    print(f"{len(metrics)} runs -> results/metrics.csv")
    out = []
    plot_exp1(metrics, out)
    plot_exp2(metrics, out)
    plot_exp3(metrics, series, out)
    plot_exp4(metrics, out)
    plot_exp5(metrics, out)
    bad = [m for m in metrics if not m["integrity"] and m["exp"] != 5]
    out.append(f"\nIntegrity failures outside experiment 5: {len(bad)}"
               + "".join(f"\n  exp{m['exp']}/{m['name']}" for m in bad))
    text = "\n".join(out)
    (ROOT / "results" / "summary.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
