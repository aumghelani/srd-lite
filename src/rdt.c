#define _POSIX_C_SOURCE 200809L

#include "rdt.h"
#include "packet.h"
#include "rtt.h"
#include "cc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MTU_PAYLOAD  1400                  /* keep packets under 1500 */
#define PKT_MAX      (PKT_HDR_SIZE + MTU_PAYLOAD)
#define TX_SLOTS     256                   /* max packets in flight */
#define PENDING_MAX  1024                  /* packets waiting for window */
#define RX_SEEN      8192                  /* psn history for dup check */
#define REASM_SLOTS  32                    /* messages being rebuilt */
#define DONE_MAX     256                   /* finished msgs not read yet */
#define MAX_RETRIES  20

/* a packet that has been sent but not acked */
typedef struct {
    int used;
    uint32_t psn;
    uint8_t buf[PKT_MAX];
    int len;
    uint64_t sent_at;
    uint64_t deadline;
    int retries;
    int path;
} tx_slot;

/* a fragment waiting for room in the window. psn is given later */
typedef struct {
    pkt_hdr h;
    uint8_t payload[MTU_PAYLOAD];
} pending_pkt;

/* a message we are putting back together */
typedef struct {
    int used;
    uint32_t msg_id;
    uint32_t msg_len;
    uint16_t frag_cnt;
    uint16_t got;
    uint8_t *have;   /* 1 byte per fragment */
    uint8_t *data;
} reasm;

typedef struct {
    uint8_t *data;
    uint32_t len;
} done_msg;

struct rdt_ep {
    int fds[RDT_MAX_PATHS];
    int npaths;
    uint16_t port;

    struct sockaddr_in peer;
    int have_peer;

    uint32_t next_msg_id;
    uint32_t next_psn;

    tx_slot *tx;
    int inflight;
    int next_path;

    pending_pkt *pend;
    int pend_head;
    int pend_count;

    uint32_t *seen;  /* seen[psn % RX_SEEN] = psn + 1 */
    reasm rx[REASM_SLOTS];

    done_msg done[DONE_MAX];
    int done_head;
    int done_count;

    rtt_est rtt;
    cc_state cc;

    double drop_rate;
    uint32_t rng;

    rdt_stats st;
};

void rdt_config_init(rdt_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->npaths = 4;
    cfg->seed = 12345;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int open_udp(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

rdt_ep *rdt_open(const rdt_config *cfg)
{
    rdt_config def;
    if (!cfg) {
        rdt_config_init(&def);
        cfg = &def;
    }
    if (cfg->npaths < 1 || cfg->npaths > RDT_MAX_PATHS)
        return NULL;

    rdt_ep *ep = calloc(1, sizeof *ep);
    if (!ep)
        return NULL;

    /* set fds to -1 first, calloc gives 0 and close(0) would kill stdin */
    for (int i = 0; i < RDT_MAX_PATHS; i++)
        ep->fds[i] = -1;

    ep->tx = calloc(TX_SLOTS, sizeof *ep->tx);
    ep->pend = calloc(PENDING_MAX, sizeof *ep->pend);
    ep->seen = calloc(RX_SEEN, sizeof *ep->seen);
    if (!ep->tx || !ep->pend || !ep->seen)
        goto fail;

    /* path 0 is the main port everyone talks to. the rest are just
     * extra source ports so the network hashes them differently */
    for (int i = 0; i < cfg->npaths; i++) {
        ep->fds[i] = open_udp(i == 0 ? cfg->port : 0);
        if (ep->fds[i] < 0)
            goto fail;
    }
    ep->npaths = cfg->npaths;

    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    getsockname(ep->fds[0], (struct sockaddr *)&a, &alen);
    ep->port = ntohs(a.sin_port);

    ep->next_msg_id = 1;
    ep->next_psn = 1;
    rtt_init(&ep->rtt);
    cc_init(&ep->cc, TX_SLOTS);
    ep->drop_rate = cfg->drop_rate;
    ep->rng = cfg->seed ? cfg->seed : 1;
    return ep;

fail:
    rdt_close(ep);
    return NULL;
}

void rdt_close(rdt_ep *ep)
{
    if (!ep)
        return;
    for (int i = 0; i < RDT_MAX_PATHS; i++)
        if (ep->fds[i] >= 0)
            close(ep->fds[i]);
    for (int i = 0; i < REASM_SLOTS; i++) {
        free(ep->rx[i].have);
        free(ep->rx[i].data);
    }
    for (int i = 0; i < ep->done_count; i++)
        free(ep->done[(ep->done_head + i) % DONE_MAX].data);
    free(ep->tx);
    free(ep->pend);
    free(ep->seen);
    free(ep);
}

uint16_t rdt_local_port(const rdt_ep *ep)
{
    return ep->port;
}

const char *rdt_strerror(int err)
{
    switch (err) {
    case RDT_ERR_SYS:    return "system error";
    case RDT_ERR_AGAIN:  return "queue full, try again";
    case RDT_ERR_TOOBIG: return "message too big";
    case RDT_ERR_NOPEER: return "no peer set";
    case RDT_ERR_INVAL:  return "invalid argument";
    default:             return "unknown error";
    }
}

int rdt_set_peer(rdt_ep *ep, const char *host, uint16_t port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;   /* ipv4 only for now */
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return RDT_ERR_INVAL;

    memcpy(&ep->peer, res->ai_addr, sizeof ep->peer);
    ep->peer.sin_port = htons(port);
    ep->have_peer = 1;
    freeaddrinfo(res);
    return 0;
}

int rdt_send(rdt_ep *ep, const void *buf, size_t len)
{
    if (!ep->have_peer)
        return RDT_ERR_NOPEER;
    if (len == 0)
        return RDT_ERR_INVAL;
    if (len > RDT_MAX_MSG)
        return RDT_ERR_TOOBIG;

    int nfrags = (len + MTU_PAYLOAD - 1) / MTU_PAYLOAD;
    if (ep->pend_count + nfrags > PENDING_MAX)
        return RDT_ERR_AGAIN;

    uint32_t id = ep->next_msg_id++;
    const uint8_t *p = buf;

    /* chop into mtu sized pieces and queue them */
    for (int i = 0; i < nfrags; i++) {
        int idx = (ep->pend_head + ep->pend_count) % PENDING_MAX;
        pending_pkt *pp = &ep->pend[idx];
        size_t off = (size_t)i * MTU_PAYLOAD;
        size_t n = len - off < MTU_PAYLOAD ? len - off : MTU_PAYLOAD;

        memset(&pp->h, 0, sizeof pp->h);
        pp->h.type = PKT_DATA;
        pp->h.msg_id = id;
        pp->h.frag_idx = i;
        pp->h.frag_cnt = nfrags;
        pp->h.msg_len = len;
        pp->h.payload_len = n;
        pp->h.src_port = ep->port;
        memcpy(pp->payload, p + off, n);
        ep->pend_count++;
    }

    ep->st.msgs_sent++;
    return 0;
}

/* every packet goes out through here so fake loss hits data and acks */
static void raw_send(rdt_ep *ep, int path, const uint8_t *buf, int len,
                     const struct sockaddr_in *to)
{
    if (ep->drop_rate > 0) {
        double r = (double)xorshift32(&ep->rng) / 4294967296.0;
        if (r < ep->drop_rate) {
            ep->st.fake_drops++;
            return;
        }
    }
    /* udp is lossy anyway, if the kernel says no we treat it as a drop */
    sendto(ep->fds[path], buf, len, 0, (const struct sockaddr *)to, sizeof *to);
    ep->st.pkts_sent++;
}

static int pick_path(rdt_ep *ep)
{
    /* round robin across source ports */
    int p = ep->next_path;
    ep->next_path = (ep->next_path + 1) % ep->npaths;
    return p;
}

/* move packets from the pending queue into flight while cwnd allows */
static void push_pending(rdt_ep *ep)
{
    uint64_t now = now_us();

    while (ep->pend_count > 0 && cc_can_send(&ep->cc, ep->inflight)) {
        uint32_t psn = ep->next_psn;
        tx_slot *t = &ep->tx[psn % TX_SLOTS];
        if (t->used)
            break; /* an old packet still holds this slot */

        pending_pkt *pp = &ep->pend[ep->pend_head];
        pp->h.psn = psn;
        t->len = pkt_encode(&pp->h, pp->payload, t->buf, sizeof t->buf);
        t->used = 1;
        t->psn = psn;
        t->retries = 0;
        t->path = pick_path(ep);
        t->sent_at = now;
        t->deadline = now + ep->rtt.rto;

        raw_send(ep, t->path, t->buf, t->len, &ep->peer);

        ep->next_psn++;
        ep->inflight++;
        ep->pend_head = (ep->pend_head + 1) % PENDING_MAX;
        ep->pend_count--;
    }
}
