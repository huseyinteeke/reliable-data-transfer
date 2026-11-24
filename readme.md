# BLG337E Project 1 — Reliable file transfer over UDP

Three C programs (C11 + POSIX only) that talk only through UDP on 127.0.0.1:

```
sender ──UDP──> emulator ──UDP──> receiver        (DATA, FIN)
sender <──UDP── emulator <──UDP── receiver        (ACK, FIN_ACK)
```

Protocols (runtime selectable): Stop-and-Wait, Go-Back-N, Selective Repeat.

## Build

```sh
cmake --preset release && cmake --build build/release     # binaries in build/release/bin
cmake --preset debug   && cmake --build build/debug       # -g3 -O0, warnings on
```

## Run one transfer by hand

```sh
B=build/release/bin
$B/emulator --listen-port 9001 --dst-port 9002 --scenario scenarios/baseline.conf --seed 1 --log logs/emulator.log &
$B/receiver --port 9002 --out /tmp/out.bin --mode sr --window 16 --k 7 --log logs/receiver.log &
$B/sender   --file testdata/input.bin --port 9001 --mode sr --window 16 --k 7 --payload 1024 --log logs/sender.log
sha256sum testdata/input.bin /tmp/out.bin
kill %1        # stop the emulator (SIGTERM flushes its log)
```

### Options (all runtime, no recompiling)

| program | option | meaning (default) |
|---|---|---|
| sender | `--file F --port P` | file to send, emulator's listen port (required) |
| | `--host H` | emulator address (127.0.0.1) |
| | `--mode sw\|gbn\|sr` | ARQ protocol (sr) |
| | `--window W`, `--k K`, `--payload N` | window (16), sequence bits 3..8 (7), max payload bytes ≤ 4096 (1024) |
| | `--rto-init/--rto-min/--rto-max MS` | RTO bounds (1000/200/8000) |
| | `--max-retx N` | give up after N retransmissions of one packet (30) |
| | `--unsafe` | allow W beyond the protocol limit (experiment 5 only) |
| | `--log PATH` | event log (logs/sender.log) |
| receiver | `--port P --out F` | listen port, output file (required) |
| | `--mode`, `--window`, `--k` | must match the sender (sr, 16, 7) |
| | `--linger MS` | keep answering FINs this long after FIN_ACK (2000) |
| | `--log PATH` | logs/receiver.log |
| emulator | `--listen-port P --dst-port Q --scenario F` | required |
| | `--seed N` | PRNG seed, overrides `seed` in the scenario file |
| | `--dst-host H`, `--log PATH`, `--idle-exit SEC` | 127.0.0.1, logs/emulator.log, never |

Window limits (checked by the sender): SW W=1, GBN W ≤ 2^k − 1, SR W ≤ 2^(k−1).

## Packet format (`config/config.h`, `config/packet.c`)

Fixed 12-byte big-endian header + payload:

| bytes | field | |
|---|---|---|
| 0 | type | 1 DATA, 2 ACK, 3 FIN, 4 FIN_ACK |
| 1 | flags | reserved, 0 |
| 2–3 | payload_len | |
| 4–7 | number | DATA: seq; ACK: ack number; FIN: seq after last DATA; FIN_ACK: same as FIN. Values are mod 2^k, field is always 32 bits |
| 8–11 | crc32 | CRC-32 (IEEE 802.3, own implementation) over header (crc=0) + payload |

Packets failing the CRC, length or range check are dropped silently.

## Protocol specification

Sequence numbers are counted mod M = 2^k; the sender keeps 64-bit absolute indices internally.

* **sw / gbn** — receiver accepts only the next expected packet and answers *every* DATA packet with a
  **cumulative ACK** = seq of the next expected packet. Sender keeps one timer (started when the window
  becomes non-empty, restarted when the base advances); on timeout it retransmits the whole window.
  SW is GBN with W = 1. A cumulative ACK with delta = (ack − base) mod M is "new" if
  1 ≤ delta ≤ packets in flight, a duplicate if delta = 0, otherwise stale.
* **sr** — receiver accepts packets with (seq − expected) mod M < W, buffers out-of-order ones, delivers in
  order, and answers each with an **individual ACK** (the packet's seq). Packets in the previous window
  (offset ≥ M − W) are re-acknowledged. Sender keeps one timer per packet and retransmits only the expired one.
* **Termination** — after the last DATA is acknowledged the sender sends FIN (number = N mod M) and retransmits
  it with the RTO until FIN_ACK arrives. The receiver answers FIN only when all data has been delivered
  (FIN number = expected seq), then lingers to re-answer a retransmitted FIN.
* **RTO** — RFC 6298: first sample SRTT=R, RTTVAR=R/2; then RTTVAR=¾RTTVAR+¼|SRTT−R|, SRTT=⅞SRTT+⅛R;
  RTO = SRTT + 4·RTTVAR, clamped to [rto-min, rto-max] (initial 1 s). Karn's rule: packets that were
  retransmitted never produce an RTT sample. On timeout the RTO is doubled and stays doubled until the next
  valid sample. For cumulative ACKs the sample comes from the last newly acknowledged packet.
  Justification: it adapts to the large/jittery RTTs of experiment 3, and Karn + backoff prevent the
  retransmission ambiguity and spurious-timeout spirals. `rto-min` 200 ms avoids spurious timeouts when
  the first window is queued behind itself at 1 Mbit/s (RTT grows by ~8 ms per packet in the queue).

## Emulator (`emulator/emulator.c`)

Scenario file: `key = value` lines (`#` comments), see `scenarios/baseline.conf`:
`rate_fwd_bps rate_rev_bps queue_fwd_pkts queue_rev_pkts delay_fwd_ms delay_rev_ms jitter_ms data_loss ack_loss corrupt duplicate reorder seed`,
plus per-packet actions `action <drop|corrupt|duplicate> <data|ack> <number>` and
`action delay <data|ack> <number> <ms>`. An action applies once, to the first packet of that kind whose on-wire
`number` equals the given value (so `action drop data 12` drops the first transmission of seq 12).

Per direction: tail-drop FIFO queue of `queue_*_pkts` packets, serialization time 8·len/rate (len = UDP payload),
then propagation delay + jitter U[−j, +j] (clamped so jitter never reorders). `data_loss` drops DATA only,
`ack_loss` drops ACKs only (FIN/FIN_ACK are never randomly lost, so runs terminate); corruption flips one random
bit; duplication delivers a copy right after the original; reordering holds a packet until the next one in
that direction has been delivered (released after 1 s if nothing follows). All random decisions come from one
PRNG seeded with the seed (a fixed number of draws per packet).

## Experiments, analysis, reproduction

```sh
python3 scripts/run_experiments.py --jobs 4        # all experiments, ~10-15 min; --exp 1 3, --seeds 1 2 3, --force, --list
python3 scripts/analyze.py                          # metrics + plots from the logs
```

* `run_experiments.py` writes, per run, `logs/exp<N>/<run>/` with `scenario.conf` (incl. seed), the three
  logs, and `meta.json` (parameters, exact command lines, exit codes, SHA-256 of input and output).
  Random experiments (2, 3, 4) are run with seeds 1, 2, 3; experiments 1 and 5 are deterministic.
  Every run uses `testdata/input.bin` (256 KiB) and the baseline of the PDF except for the listed parameters.
* `analyze.py` writes `results/metrics.csv`, `results/summary.txt` (tables with theory columns) and `plot/*.png`
  (`exp1_ideal`, `exp2_loss`, `exp3_delay`, `exp3_rto_tracking`, `exp4_impairments`, `exp5_seqlimit`).
  Metric definitions are in the docstring of `scripts/analyze.py`.
* Experiment 5 (`--unsafe`, `--max-retx 5 --rto-max 1000`) uses deterministic per-packet actions:
  GBN drops the first ACKs numbered 1..M−1, SR drops the first ACK numbered 0, for k = 3 and 4 and every W = 1..M.
  GBN fails exactly at W = M (the ACK for a full window equals the duplicate-ACK value), SR exactly at W > M/2
  (a retransmitted old packet falls into the receiver's new window and is accepted as new data).

### Log formats (CSV, `t_us` = absolute CLOCK_MONOTONIC µs, one time axis for all three programs)

* sender: `t_us,event,type,seq,abs,len,base,next,rto_us,rtt_us,note` — events START, SEND, RESEND, RECV_ACK, RECV_BAD, RECV_IGN, TIMEOUT, DONE
* receiver: `t_us,event,type,seq,abs,len,expected,note`
* emulator: `t_us,event,dir,type,number,len,note` — events ARRIVE, DROP, DELAY, CORRUPT, HOLD, DUPLICATE, DELIVER

## Repository layout

`config/` packet format + CRC-32 · `sender/ receiver/ emulator/` programs · `scenarios/` · `scripts/` · `testdata/` ·
`logs/` run logs · `results/` metrics · `plot/` figures
