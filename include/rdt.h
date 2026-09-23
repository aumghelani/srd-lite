#ifndef RDT_H
#define RDT_H

/*
 * rdt - reliable datagram transport over udp
 *
 * messages are delivered reliably but NOT in order. each message
 * shows up as soon as all of its fragments have arrived.
 * packets are sprayed across several udp source ports (paths).
 */

#include <stddef.h>
#include <stdint.h>

#define RDT_MAX_MSG    (1 << 20)  /* 1 MB */
#define RDT_MAX_PATHS  8

/* error codes, all negative */
#define RDT_ERR_SYS     -1  /* check errno */
#define RDT_ERR_AGAIN   -2  /* queue full, call rdt_progress and retry */
#define RDT_ERR_TOOBIG  -3
#define RDT_ERR_NOPEER  -4
#define RDT_ERR_INVAL   -5

typedef struct rdt_ep rdt_ep;

typedef struct {
    uint16_t port;      /* local port, 0 = pick any */
    int npaths;         /* how many source ports to spray over */
    double drop_rate;   /* fake loss on send, for testing. 0 = off */
    uint32_t seed;      /* rng seed for the fake loss */
} rdt_config;

typedef struct {
    uint64_t msgs_sent;
    uint64_t msgs_recv;
    uint64_t pkts_sent;
    uint64_t pkts_recv;
    uint64_t retransmits;
    uint64_t dups;          /* duplicate packets we threw away */
    uint64_t bad_pkts;      /* failed checksum etc */
    uint64_t fake_drops;
    uint64_t give_ups;      /* packets dropped after too many retries */
    double cwnd;
    uint64_t srtt_us;
    uint64_t rto_us;
} rdt_stats;

void    rdt_config_init(rdt_config *cfg);
rdt_ep *rdt_open(const rdt_config *cfg);
void    rdt_close(rdt_ep *ep);

int      rdt_set_peer(rdt_ep *ep, const char *host, uint16_t port);
uint16_t rdt_local_port(const rdt_ep *ep);

/* queue a message. data is copied so buf can be reused right away */
int rdt_send(rdt_ep *ep, const void *buf, size_t len);

/* does all the io: read packets, send acks, retransmit.
 * waits up to timeout_ms for something to happen */
int rdt_progress(rdt_ep *ep, int timeout_ms);

/* pop one finished message. returns length, 0 if nothing ready */
long rdt_recv(rdt_ep *ep, void *buf, size_t cap);

/* 1 if everything we sent has been acked */
int rdt_idle(const rdt_ep *ep);

void rdt_get_stats(const rdt_ep *ep, rdt_stats *st);
const char *rdt_strerror(int err);

#endif
