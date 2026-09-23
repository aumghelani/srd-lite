# rdt

A reliable datagram transport over UDP, written in C. Loosely based on the
ideas behind AWS SRD (the protocol under EFA):

- **reliable but unordered**: each message is handed to the app as soon as it's
  complete, so one lost packet doesn't block everything behind it
- **multipath**: packets are sprayed across several UDP source ports, and
  retransmits switch to a different path
- **fast recovery**: 1ms min RTO (TCP uses 200ms), plus fast retransmit
- AIMD congestion control, RTT estimation, checksums, fragmentation up to 1 MB

No dependencies. Builds on Linux and macOS.

See [DESIGN.md](DESIGN.md) for how it works and the bugs I ran into.

## build

```sh
make          # library, tests, tools -> build/
make test     # run all tests
make asan     # tests again with address + undefined sanitizers
```

## usage

```c
#include "rdt.h"

rdt_config cfg;
rdt_config_init(&cfg);
cfg.npaths = 4;

rdt_ep *ep = rdt_open(&cfg);
rdt_set_peer(ep, "10.0.0.2", 9000);

rdt_send(ep, "hello", 5);          /* copies, returns right away */

char buf[RDT_MAX_MSG];
for (;;) {
    rdt_progress(ep, 10);          /* all io happens here */
    long n = rdt_recv(ep, buf, sizeof buf);
    if (n > 0)
        printf("got %ld bytes\n", n);
}
```

Nothing happens in the background. You have to keep calling `rdt_progress`,
same as polling a completion queue in libfabric.

## tools

```sh
./build/rdt_echo -p 9000                       # echo server
./build/rdt_bench -p 9000 -n 20000             # ping latency
./build/rdt_bench -p 9000 -m stream -s 8000    # throughput
./build/rdt_bench -p 9000 -l 0.01              # with 1% fake loss

./build/tcp_bench -S -p 9001                   # tcp baseline server
./build/tcp_bench -p 9001 -n 20000             # tcp baseline client
```

`-l` drops packets inside rdt itself, so it works anywhere. For real loss
on Linux use netem:

```sh
sudo ./scripts/netem.sh on 1% 50us    # 1% loss on loopback
sudo ./scripts/compare.sh             # rdt vs tcp at 0..5% loss
sudo ./scripts/netem.sh off
```

## results

macOS, Apple Silicon, loopback, 4 paths, fake loss on both sides.

**ping, 64 byte messages, 20k round trips**

| loss | p50 | p99 | p999 |
|---|---|---|---|
| 0%   | 44 us | 79 us   | 104 us  |
| 0.1% | 43 us | 81 us   | 1.2 ms  |
| 1%   | 44 us | 1.2 ms  | 2.4 ms  |
| 5%   | 90 us | 1.5 ms  | 3.7 ms  |

For scale, TCP's minimum RTO on Linux is 200 ms, so a single lost
packet puts it at hundreds of ms. Run `scripts/compare.sh` on Linux to see that side by side.

**stream, 8000 byte messages**

| loss | throughput |
|---|---|
| 0%  | 118 MB/s |
| 1%  | 113 MB/s |
| 5%  | 55 MB/s  |

Throughput is mostly limited by one syscall per packet right now (see DESIGN.md).

## layout

```
include/rdt.h      public api
src/packet.c       header encode/decode, checksum
src/rtt.c          rtt / rto estimator
src/cc.c           aimd congestion control
src/rdt.c          endpoint, send/recv, retransmits, reassembly
tests/             unit tests + loopback test with loss
tools/             echo server, bench, tcp baseline
scripts/           netem helpers
```

## license

MIT

---

Aum Ghelani · [github.com/aumghelani](https://github.com/aumghelani)
