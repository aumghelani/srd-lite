# srd-lite

A reliable, unordered, multipath datagram transport over UDP in C11, inspired by AWS Scalable Reliable Datagram (SRD).

[![ci](https://github.com/aumghelani/srd-lite/actions/workflows/ci.yml/badge.svg)](https://github.com/aumghelani/srd-lite/actions/workflows/ci.yml)
![C11](https://img.shields.io/badge/language-C11-blue)
![license](https://img.shields.io/badge/license-MIT-lightgrey)

## Overview

AWS SRD, the protocol underneath the Elastic Fabric Adapter (EFA) used for HPC and ML clusters, drops TCP's in-order byte stream in favor of reliable but unordered delivery, sprays packets across many network paths, and retransmits on a microsecond scale. srd-lite is a small learning implementation of those ideas on top of UDP, built to measure how they affect tail latency under packet loss. It includes a TCP baseline for comparison.

| | TCP | srd-lite |
|---|---|---|
| Ordering | one strict byte stream | none; each message is delivered as soon as it is complete |
| One lost packet | blocks everything behind it (head-of-line blocking) | delays only its own message |
| Paths | one flow uses one path | packets sprayed round-robin across N UDP source ports |
| Minimum retransmit timeout | 200 ms on Linux | 1 ms |
| Retransmission path | same path | a different path |

In a datacenter the round trip is tens of microseconds, so a 200 ms timeout means one lost packet costs thousands of RTTs. Collective operations such as allreduce wait for their slowest message, which makes p99 and p999 latency the numbers that matter.

## Key Features

- **Reliable, unordered messages** up to 1 MB, fragmented into 1400-byte payloads and reassembled (up to 32 messages in flight at the receiver)
- **Multipath spraying** round-robin across up to 8 UDP source ports; retransmissions move to a different path
- **Per-packet selective acknowledgements** and duplicate detection
- **RTT estimation** in the style of RFC 6298 (`srtt + 4*rttvar`, clamped to 1 ms..2 s) with Karn's rule and per-packet exponential backoff
- **Fast retransmit** tuned to tolerate multipath reordering (a packet is considered lost only when an ACK arrives 32+ sequence numbers ahead and at least one SRTT has passed)
- **AIMD congestion control** with slow start and a NewReno-style recovery point so the window is cut once per loss window
- **FNV-1a checksum** over header and payload on a hand-packed 28-byte big-endian header
- **Backpressure**: when the receiver's completion queue is full it drops packets without ACKing, so the sender retries instead of data being lost
- **Peer learning**: a server learns its peer from the first packet, no `rdt_set_peer` needed
- **Built-in simulated loss** (`drop_rate`) for testing on any OS, plus `tc netem` scripts for real loss on Linux
- **Per-path statistics**, an echo server, a latency/throughput benchmark, and a TCP baseline benchmark
- Single-threaded manual progress model (like libfabric), no locks, no third-party dependencies

## Architecture / How It Works

```
 sender                                                   receiver
 ------                                                   --------
 rdt_send()                                               rdt_recv()
    |                                                        ^
    v                                                        |
 pending queue                                            done queue  <- complete message,
    |  (cwnd allows?)                                        ^           delivered immediately
    v                                                        |
 tx slots [psn % 256] ---- path 0 ---->  +---------+      reassembly (32 msgs)
   |  ^                ---- path 1 ---->  | network |  -->    ^
   |  |                ---- path 2 ---->  +---------+        |
   |  |                ---- path 3 ---->                  dup check (seen[psn % 8192])
   |  +------------------- ACK (per packet) ----------------+
   |
   +-- timeout / fast retransmit --> resend on the next path
```

All I/O happens inside `rdt_progress()`:

1. Move pending fragments into transmit slots while the congestion window allows
2. Poll all path sockets, with the sleep capped at the next retransmit deadline
3. Read packets; handle DATA (dedupe, reassemble, ACK) and ACKs (free slot, RTT sample, congestion control)
4. Fire retransmit timers and fast retransmits
5. Push pending fragments again, since ACKs may have opened the window

A packet still unacknowledged after 20 retransmissions is abandoned and counted in `give_ups`. The wire format, congestion control details, tradeoffs, and a log of bugs found during development are in [DESIGN.md](DESIGN.md).

## Tech Stack

- C11
- POSIX sockets (UDP, TCP), `poll`, `clock_gettime`
- GNU Make
- GCC, Clang
- AddressSanitizer, UndefinedBehaviorSanitizer
- Linux `tc` / netem (loss and delay injection)
- GitHub Actions (Ubuntu and macOS)
- Domain: network transport protocols, reliable datagrams, multipath, congestion control (AIMD), RTT estimation (RFC 6298), HPC networking

## Getting Started

Requirements: Linux or macOS, a C11 compiler (gcc or clang), `make`, and `ar`. No third-party dependencies.

```sh
git clone https://github.com/aumghelani/srd-lite.git
cd srd-lite

make          # static library (build/librdt.a), tests and tools -> build/
make test     # run the test suite
make asan     # rebuild with AddressSanitizer + UBSan and run the tests
make clean
```

### Tools

```sh
./build/rdt_echo -p 9000                        # echo server (-P paths, -l fake loss rate)
./build/rdt_bench -p 9000 -n 20000              # ping-pong latency (p50/p99/p999/max)
./build/rdt_bench -p 9000 -m stream -s 8000     # throughput
./build/rdt_bench -p 9000 -l 0.01               # with 1% simulated loss

./build/tcp_bench -S -p 9001                    # TCP baseline server
./build/tcp_bench -p 9001 -n 20000              # TCP baseline client
```

`rdt_bench` also accepts `-h host` and `-P npaths`. Real loss on Linux with netem (requires root):

```sh
sudo ./scripts/netem.sh on 1% 50us    # 1% loss + 50 us delay on loopback
sudo ./scripts/compare.sh             # srd-lite vs TCP at 0 / 0.1 / 1 / 2 / 5% loss -> results.txt
sudo ./scripts/netem.sh off
```

### Usage

```c
#include "rdt.h"

rdt_config cfg;
rdt_config_init(&cfg);
cfg.npaths = 4;                      /* spray over 4 source ports (the default) */

rdt_ep *ep = rdt_open(&cfg);
rdt_set_peer(ep, "10.0.0.2", 9000);

rdt_send(ep, "hello", 5);            /* copies the data and returns immediately */

char buf[RDT_MAX_MSG];
for (;;) {
    rdt_progress(ep, 10);            /* all I/O happens here */
    long n = rdt_recv(ep, buf, sizeof buf);
    if (n > 0)
        printf("got %ld bytes\n", n);
}
```

Link against `build/librdt.a` with `-Iinclude`. Errors are negative `RDT_ERR_*` codes; `rdt_strerror()` converts them to text.

## Benchmarks

Recorded on Apple Silicon over loopback, 4 paths, with simulated loss (`drop_rate`) applied on both ends. Loopback numbers reflect protocol behavior, not datacenter network performance.

Ping-pong, 64-byte messages, 20,000 round trips:

| loss | p50 | p99 | p999 |
|---|---|---|---|
| 0% | 44 us | 79 us | 104 us |
| 0.1% | 43 us | 81 us | 1.2 ms |
| 1% | 44 us | 1.2 ms | 2.4 ms |
| 5% | 90 us | 1.5 ms | 3.7 ms |

Stream, 8000-byte messages:

| loss | throughput |
|---|---|
| 0% | 118 MB/s |
| 1% | 113 MB/s |
| 5% | 55 MB/s |

Throughput is currently limited by one syscall per packet.

Effect of individual changes measured during development (same setup):

| Change | Before | After |
|---|---|---|
| Read ACKs before checking timers | 385 spurious retransmits at 0% loss | 0 |
| Cap poll sleep at the next retransmit deadline | p99 at 1% loss: 10.3 ms | 5.8 ms |
| Lower RTO floor from 5 ms to 1 ms | p99 at 1% loss: 5.8 ms | 1.2 ms |
| Cut cwnd once per window instead of per loss | 5% loss test: 630 ms | 340 ms |

## Project Structure

```
srd-lite/
├── include/rdt.h          public API
├── src/
│   ├── rdt.c              endpoint: send, recv, progress, retransmit, reassembly
│   ├── packet.c / .h      wire format encode/decode, FNV-1a checksum
│   ├── rtt.c / .h         RTT / RTO estimator
│   ├── cc.c / .h          AIMD congestion control
│   └── util.h             sequence-number comparison, clock, RNG helpers
├── tests/                 unit and loopback tests
├── tools/
│   ├── rdt_echo.c         echo server
│   ├── rdt_bench.c        latency / throughput benchmark
│   └── tcp_bench.c        TCP baseline
├── scripts/
│   ├── netem.sh           enable/disable netem loss on loopback
│   └── compare.sh         srd-lite vs TCP across loss rates
├── .github/workflows/ci.yml
├── DESIGN.md              design notes, tradeoffs, bug log
└── Makefile
```

## Testing

```sh
make test
```

| Test | Coverage |
|---|---|
| `test_packet` | encode/decode round trip, short buffers, bad magic, undersized encode buffer, corrupted payload |
| `test_rtt` | initial RTO, first sample, clamping, convergence |
| `test_cc` | slow start, halving on loss, one cut per RTT, floor and cap, send gating |
| `test_util` | sequence-number wraparound, RNG, monotonic clock |
| `test_loopback` | two endpoints over loopback with 1 and 4 paths at 0%, 2%, 5% and 20% simulated loss, including large messages; verifies every message arrives exactly once and byte-for-byte intact, and that every path is used |

CI (GitHub Actions) builds and tests on Ubuntu with gcc and clang and on macOS with clang, both plain and with AddressSanitizer + UBSan.

## Limitations / Roadmap

Current limitations:

- One peer per endpoint, IPv4 only
- No connection setup/teardown and no encryption
- The duplicate-detection window is fixed (8192 sequence numbers); a duplicate arriving later than that would be accepted again
- Loss-based AIMD treats random loss as congestion (cwnd stays around 5 at 5% simulated loss)
- With a 1 ms RTO floor, a few timers fire early when a full window builds a queue
- One `sendto`/`recvfrom` per packet, which likely caps loopback throughput

Ideas for future work (from [DESIGN.md](DESIGN.md)):

- Delay-based congestion control, compared against AIMD at the same loss rates
- Per-path RTT/loss tracking to stop using lossy paths
- Syscall batching with `sendmmsg`/`recvmmsg`
- Zero-copy send

## References

- L. Shalev et al., "A Cloud-Optimized Transport Protocol for Elastic and Scalable HPC", IEEE Micro, 2020 (the SRD paper)
- RFC 6298, "Computing TCP's Retransmission Timer"
- [libfabric](https://github.com/ofiwg/libfabric), whose manual progress model this follows
- [mrcache](https://github.com/aumghelani/mrcache): a memory registration cache for RDMA-style networking

## License

MIT. See [LICENSE](LICENSE).
