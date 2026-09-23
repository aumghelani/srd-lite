# rdt design notes

## the problem

TCP gives you one ordered byte stream. if packet 5 is lost, packets 6..100
can already be sitting in the receiver but the app cant have them until 5
is retransmitted. thats head-of-line blocking, and in a datacenter it shows
up as tail latency (p99, p999), which is what hurts ML/HPC jobs because
a collective is only as fast as its slowest message.

TCP also has a min retransmit timeout of 200ms on linux. datacenter rtts
are tens of microseconds, so one lost packet costs ~1000x the normal rtt.

AWS's SRD protocol (used by EFA) takes a different approach:

- reliable, but **no ordering**. deliver whatever is complete
- **spray** packets across many network paths instead of pinning a flow to one
- retransmit fast, in microseconds not milliseconds

rdt is a small version of those ideas on top of UDP so i can see how they behave.

## packet format

28 byte header, big endian, packed by hand (no struct casting, so padding and
endianness dont matter):

```
 0      2    3    4         8         12      14      16        20      22      24        28
 +------+----+----+---------+---------+-------+-------+---------+-------+-------+---------+
 |magic |ver |type| msg_id  |   psn   | frag  | frag  | msg_len |payload| src   | checksum|
 | "RD" |    |    |         |         |  idx  |  cnt  |         |  len  | port  | fnv1a   |
 +------+----+----+---------+---------+-------+-------+---------+-------+-------+---------+
```

- `psn` - packet sequence number. unique per packet, retransmits reuse it.
  acks and dup detection work on psn
- `msg_id` + `frag_idx` + `frag_cnt` - for rebuilding messages
- `src_port` - sender's main port. extra paths use random source ports, so
  the receiver needs this to know where to reply to
- `checksum` - fnv-1a over header + payload. udp has its own checksum but
  its optional in ipv4 and often offloaded, so i do my own

two packet types: DATA and ACK. every DATA packet is acked on its own
(selective ack), there is no cumulative ack.

## sender

```
rdt_send() --> pending queue --(cwnd allows?)--> tx slots (in flight) --> udp
                                                   |   ^
                                        timeout /  |   | ack frees slot
                                        fast retx  v   |
                                                  resend on next path
```

- messages get chopped into 1400 byte fragments and go in the **pending queue**.
  no psn yet
- `push_pending` gives each one a psn and puts it in `tx[psn % 256]` as long
  as cwnd allows. if that slot is still busy with an old packet we just wait
- path = round robin over the source sockets

### retransmits

- **timer**: each packet has a deadline. rto is computed like rfc 6298
  (`srtt + 4*rttvar`), clamped to [1ms, 2s], with exponential backoff per packet
- **fast retransmit**: if an ack arrives for a psn more than 32 ahead of an
  unacked packet, and at least one srtt has passed, that packet is probably
  lost. resend without waiting for the timer. the gap is big because spraying
  over paths reorders packets and reordering isnt loss
- retransmits go out on a **different path** than last time. if one path is
  broken we dont keep hitting it
- **karn's rule**: no rtt samples from retransmitted packets, since you cant
  tell which copy the ack is for

### congestion control

AIMD, window counted in packets:
- slow start: +1 per ack until ssthresh
- then +1/cwnd per ack (about +1 per rtt)
- on loss: halve, min 2

the cut happens **once per window** (newreno style recovery point). the first
version cut once per srtt instead, but loopback srtt is ~50us, so it
effectively cut on every single loss and cwnd dropped to about 2.

known weakness: loss based cc treats random loss like congestion. at 5% fake
loss cwnd sits around 5. real SRD uses delay/rtt based signals, which is
the obvious next thing to try.

## receiver

- **dup check**: `seen[psn % 8192] = psn`. if we already saw it, ack again
  (the first ack might have been lost) and drop it. psn 0 is never used so
  0 can mean empty
- **reassembly**: up to 32 messages being rebuilt at once, one byte per
  fragment to track what arrived
- **out of order delivery**: when a message has all fragments it goes into
  the done queue right away, whether or not older messages are done
- **backpressure**: if the done queue is full (app isnt reading) we drop
  the packet *without acking*, so the sender retries later instead of us
  losing data
- **peer learning**: a server doesnt need `rdt_set_peer`, it learns the
  peer from the first packet (source ip + `src_port`)

## progress model

one thread, no background threads. the app calls `rdt_progress()`, which:

1. pushes pending packets
2. polls all path sockets (timeout capped at the next retransmit deadline)
3. reads packets, handles data/acks
4. checks timers, retransmits
5. pushes pending again (acks may have opened the window)

this is the same model libfabric uses (manual progress, the app drives
the completion queue). no locks needed.

## bugs i hit (and what they taught me)

| bug | symptom | fix |
|---|---|---|
| decode only hashed the header | every packet failed checksum | hash header + payload on both sides |
| `fds[]` set to -1 after a possible `goto fail` | a failed open would `close(0)` | init fds first |
| timers checked before reading socket | 385 retransmits with **0% loss** | read acks first, then check timers |
| read budget 64 < window 256 | 1-path case still had spurious retx | budget = window size |
| cc cut once per srtt | cwnd collapsed under loss, fast retx made it *slower* | once per window |
| poll ignored retransmit deadlines | p99 = 10ms (the server's poll timeout) | cap poll timeout at next deadline |
| makefile had no header deps | changed RTO_MIN but binaries kept the old value | `-MMD -MP` |
| busy loop in a test used `int` | ubsan: signed overflow | `unsigned` |

## tradeoffs / limitations

- RTO_MIN 1ms: good for tail latency, but when a full window builds a queue
  srtt gets close to 1ms and a few timers fire early (~20 spurious retx out of
  ~30k packets in the stream test). tcp picks 200ms to never do that
- one peer per endpoint, ipv4 only
- `seen[]` is a fixed window. a duplicate that shows up more than 8192
  packets late would be accepted again. fine for a lab, not for production
- no connection setup/teardown, no encryption
- every packet is its own `sendto`/`recvfrom`. `sendmmsg`/`recvmmsg` or
  io_uring would cut syscalls a lot. thats probably what caps throughput
  at ~118 MB/s on loopback

## ideas for later

- delay based congestion control (compare to AIMD at the same loss rates)
- per path rtt/loss stats, stop using a path that keeps losing
- batch syscalls with `sendmmsg`/`recvmmsg`
- zero copy send (hand the user's buffer to the socket, no copy into pending)
