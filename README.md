# srd-lite

[![ci](https://github.com/aumghelani/srd-lite/actions/workflows/ci.yml/badge.svg)](https://github.com/aumghelani/srd-lite/actions/workflows/ci.yml)
![language](https://img.shields.io/badge/language-C11-blue)
![deps](https://img.shields.io/badge/dependencies-none-brightgreen)
![license](https://img.shields.io/badge/license-MIT-lightgrey)

A small reliable datagram transport over UDP, written in C, inspired by
the ideas behind AWS **SRD** (Scalable Reliable Datagram), the protocol
underneath the Elastic Fabric Adapter used for ML and HPC clusters.

It's a learning project. I wanted to see for myself *why* a datacenter
transport would drop TCP's in-order delivery, and what spraying packets
across many paths does to tail latency.

```
 1% packet loss, 64 byte ping-pong

            p50      p99       p999
 srd-lite   44 us    1.2 ms    2.4 ms
```

---

## why not just tcp?

| | TCP | srd-lite |
|---|---|---|
| ordering | one strict byte stream | none, each message delivered when complete |
| one lost packet | blocks everything behind it (head-of-line blocking) | only delays its own message |
| paths | one flow = one path | packets sprayed across N source ports |
| min retransmit timeout | 200 ms on Linux | 1 ms |
| retransmit goes out on | same path | a different path |

In a datacenter the round trip is tens of microseconds, so a 200 ms timeout
means one lost packet costs thousands of RTTs. Collective ops (allreduce etc.)
wait for their slowest message, so the tail (p99, p999) is what matters.

## features

- **reliable, unordered messages** up to 1 MB, fragmented into 1400 byte packets
- **multipath spraying**: round robin across several UDP source ports,
  retransmits switch path
- **per-packet selective acks** and duplicate detection
- **RTT estimation** (RFC 6298 style) with Karn's rule and exponential backoff
- **fast retransmit** that tolerates reordering from multipath
- **AIMD congestion control** with a NewReno style recovery point
- **fnv-1a checksum** over header + payload
- **backpressure**: a full receiver drops without acking, so nothing is lost
- **built-in fake loss** (`drop_rate`) for testing anywhere, plus `netem` scripts for real loss on Linux
- **per-path stats**, a benchmark tool, and a **TCP baseline** to compare against
- single-threaded, manual progress model (like libfabric), no locks, no dependencies

## how it works

```
 sender                                                   receiver
 ------                                                   --------
 rdt_send()                                               rdt_recv()
    |                                                        ^
    v                                                        |
 pending queue                                            done queue  <- message complete,
    |  (cwnd allows?)                                        ^           deliver right away
    v                                                        |
 tx slots [psn % 256] ---- path 0 ---->  +--------+        reassembly (32 msgs)
   |  ^                ---- path 1 ---->  | network |  -->    ^
   |  |                ---- path 2 ---->  +--------+         |
   |  |                ---- path 3 ---->                  dup check (seen[psn])
   |  +------------------- ACK (per packet) ----------------+
   |
   +-- timeout / fast retx --> resend on the next path
```

All I/O happens inside `rdt_progress()`: push pending packets, poll the path
sockets (sleep capped at the next retransmit deadline), handle data and acks,
fire timers. The full write-up is in **[DESIGN.md](DESIGN.md)**, including the
wire format, the congestion control, the tradeoffs, and a table of the bugs I hit
and what they taught me.

## build

Linux or macOS, any C11 compiler.

```sh
make          # library + tests + tools into build/
make test     # run the test suite
make asan     # rebuild with address + undefined sanitizers and test again
```

## usage

```c
#include "rdt.h"

rdt_config cfg;
rdt_config_init(&cfg);
cfg.npaths = 4;                      /* spray over 4 source ports */

rdt_ep *ep = rdt_open(&cfg);
rdt_set_peer(ep, "10.0.0.2", 9000);

rdt_send(ep, "hello", 5);            /* copies the data, returns right away */

char buf[RDT_MAX_MSG];
for (;;) {
    rdt_progress(ep, 10);            /* all io happens here */
    long n = rdt_recv(ep, buf, sizeof buf);
    if (n > 0)
        printf("got %ld bytes\n", n);
}
```

A server doesn't need `rdt_set_peer`. It learns the peer from the first packet.

## tools

```sh
./build/rdt_echo -p 9000                        # echo server
./build/rdt_bench -p 9000 -n 20000              # ping-pong latency
./build/rdt_bench -p 9000 -m stream -s 8000     # throughput
./build/rdt_bench -p 9000 -l 0.01               # with 1% fake loss

./build/tcp_bench -S -p 9001                    # tcp baseline server
./build/tcp_bench -p 9001 -n 20000              # tcp baseline client
```

Real loss on Linux with netem:

```sh
sudo ./scripts/netem.sh on 1% 50us    # 1% loss + 50us delay on loopback
sudo ./scripts/compare.sh             # srd-lite vs tcp at 0 / 0.1 / 1 / 2 / 5% loss
sudo ./scripts/netem.sh off
```

## results

Apple Silicon, loopback, 4 paths, fake loss applied on both ends.

**Ping-pong, 64 byte messages, 20k round trips**

| loss | p50 | p99 | p999 |
|---|---|---|---|
| 0%   | 44 us | 79 us   | 104 us |
| 0.1% | 43 us | 81 us   | 1.2 ms |
| 1%   | 44 us | 1.2 ms  | 2.4 ms |
| 5%   | 90 us | 1.5 ms  | 3.7 ms |

**Stream, 8000 byte messages**

| loss | throughput |
|---|---|
| 0%  | 118 MB/s |
| 1%  | 113 MB/s |
| 5%  | 55 MB/s  |

Throughput is limited by one syscall per packet for now. `sendmmsg`/`recvmmsg`
batching is on the list.

**Improvements I measured along the way**

| change | before | after |
|---|---|---|
| read acks before checking timers | 385 spurious retransmits at 0% loss | 0 |
| cap poll sleep at next retransmit deadline | p99 at 1% loss = 10.3 ms | 5.8 ms |
| RTO floor 5 ms → 1 ms | p99 at 1% loss = 5.8 ms | 1.2 ms |
| cut cwnd once per window, not per loss | 5% loss test: 630 ms | 340 ms |

## tests

- `test_packet`: encode/decode round trip, bad magic, short buffers, flipped bits
- `test_rtt`: RTO math, clamping, convergence
- `test_cc`: slow start, halving, one cut per RTT, floor and cap
- `test_util`: sequence number wraparound
- `test_loopback`: two endpoints, thousands of random size messages at 0, 2, 5 and 20% loss,
  checks every message arrives **exactly once** and byte-for-byte intact, and every path is used

CI runs everything on Ubuntu and macOS with gcc and clang, plain and with sanitizers.

## layout

```
include/rdt.h        public api
src/packet.c         wire format, checksum
src/rtt.c            rtt / rto estimator
src/cc.c             congestion control
src/rdt.c            endpoint: send, recv, retransmit, reassembly
tests/               unit + loopback tests
tools/               echo server, benchmark, tcp baseline
scripts/             netem helpers
DESIGN.md            how it works and why
```

## limitations

One peer per endpoint, IPv4 only, no connection handshake or encryption,
fixed-size duplicate window. See [DESIGN.md](DESIGN.md#tradeoffs--limitations).

## references

- L. Shalev et al., *"A Cloud-Optimized Transport Protocol for Elastic and Scalable HPC"*, IEEE Micro 2020 (the SRD paper)
- RFC 6298, *Computing TCP's Retransmission Timer*
- [libfabric](https://github.com/ofiwg/libfabric), whose manual progress model this follows

## license

MIT © Aum Ghelani · [github.com/aumghelani](https://github.com/aumghelani)
