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
#define REORDER_GAP  32                    /* how far behind before we call it lost */

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

    uint32_t max_acked;  /* highest psn acked so far */

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

static void handle_ack(rdt_ep *ep, const pkt_hdr *h)
{
    tx_slot *t = &ep->tx[h->psn % TX_SLOTS];
    if (!t->used || t->psn != h->psn)
        return; /* old or duplicate ack */

    /* karn: dont trust rtt from a packet we sent more than once */
    if (t->retries == 0)
        rtt_sample(&ep->rtt, now_us() - t->sent_at);

    if (h->psn > ep->max_acked)
        ep->max_acked = h->psn;

    t->used = 0;
    ep->inflight--;
    cc_on_ack(&ep->cc);
}

static void send_ack(rdt_ep *ep, int path, uint32_t psn, const struct sockaddr_in *to)
{
    pkt_hdr h;
    uint8_t buf[PKT_HDR_SIZE];
    memset(&h, 0, sizeof h);
    h.type = PKT_ACK;
    h.psn = psn;
    h.src_port = ep->port;
    int n = pkt_encode(&h, NULL, buf, sizeof buf);
    raw_send(ep, path, buf, n, to);
}

static reasm *find_reasm(rdt_ep *ep, const pkt_hdr *h)
{
    reasm *free_slot = NULL;
    for (int i = 0; i < REASM_SLOTS; i++) {
        reasm *r = &ep->rx[i];
        if (r->used && r->msg_id == h->msg_id)
            return r;
        if (!r->used && !free_slot)
            free_slot = r;
    }
    if (!free_slot)
        return NULL;

    /* first fragment of a new message */
    free_slot->have = calloc(h->frag_cnt, 1);
    free_slot->data = malloc(h->msg_len);
    if (!free_slot->have || !free_slot->data) {
        free(free_slot->have);
        free(free_slot->data);
        free_slot->have = NULL;
        free_slot->data = NULL;
        return NULL;
    }
    free_slot->used = 1;
    free_slot->msg_id = h->msg_id;
    free_slot->msg_len = h->msg_len;
    free_slot->frag_cnt = h->frag_cnt;
    free_slot->got = 0;
    return free_slot;
}

static void handle_data(rdt_ep *ep, int path, const pkt_hdr *h,
                        const uint8_t *payload, const struct sockaddr_in *from)
{
    /* reply goes back to the exact port it came from (their path socket) */
    uint32_t *seen = &ep->seen[h->psn % RX_SEEN];
    if (*seen == h->psn + 1) {
        /* already have it. our ack probably got lost, so ack again */
        ep->st.dups++;
        send_ack(ep, path, h->psn, from);
        return;
    }

    /* sanity checks, dont trust the wire */
    size_t off = (size_t)h->frag_idx * MTU_PAYLOAD;
    if (h->frag_cnt == 0 || h->frag_idx >= h->frag_cnt ||
        h->msg_len > RDT_MAX_MSG || off + h->payload_len > h->msg_len) {
        ep->st.bad_pkts++;
        return;
    }

    /* no room to deliver. drop without acking so they resend later */
    if (ep->done_count == DONE_MAX)
        return;

    reasm *r = find_reasm(ep, h);
    if (!r)
        return;
    if (r->msg_len != h->msg_len || r->frag_cnt != h->frag_cnt) {
        ep->st.bad_pkts++;
        return;
    }

    if (!r->have[h->frag_idx]) {
        memcpy(r->data + off, payload, h->payload_len);
        r->have[h->frag_idx] = 1;
        r->got++;
    }
    *seen = h->psn + 1;
    send_ack(ep, path, h->psn, from);

    /* learn who to talk back to if nobody told us */
    if (!ep->have_peer) {
        ep->peer = *from;
        ep->peer.sin_port = htons(h->src_port);
        ep->have_peer = 1;
    }

    if (r->got == r->frag_cnt) {
        /* whole message is here. hand it over, dont wait for older ones */
        done_msg *d = &ep->done[(ep->done_head + ep->done_count) % DONE_MAX];
        d->data = r->data;
        d->len = r->msg_len;
        ep->done_count++;
        ep->st.msgs_recv++;

        free(r->have);
        r->have = NULL;
        r->data = NULL;
        r->used = 0;
    }
}

/* packets way behind the newest ack are probably lost. paths reorder
 * stuff so the gap has to be big-ish, and give it at least one srtt */
static int looks_lost(const rdt_ep *ep, const tx_slot *t, uint64_t now)
{
    return t->retries == 0 &&
           t->psn + REORDER_GAP < ep->max_acked &&
           now - t->sent_at > (uint64_t)ep->rtt.srtt;
}

static void check_timeouts(rdt_ep *ep)
{
    uint64_t now = now_us();

    for (int i = 0; i < TX_SLOTS; i++) {
        tx_slot *t = &ep->tx[i];
        if (!t->used)
            continue;

        int fast = looks_lost(ep, t, now);
        if (!fast && now < t->deadline)
            continue;
        if (fast)
            ep->st.fast_retx++;

        if (t->retries >= MAX_RETRIES) {
            /* peer is probably gone. stop trying */
            t->used = 0;
            ep->inflight--;
            ep->st.give_ups++;
            continue;
        }

        t->retries++;
        /* try a different path, the old one might be the broken one */
        t->path = (t->path + 1) % ep->npaths;

        /* exponential backoff */
        uint64_t rto = ep->rtt.rto << t->retries;
        if (rto > RTO_MAX)
            rto = RTO_MAX;
        t->deadline = now + rto;

        raw_send(ep, t->path, t->buf, t->len, &ep->peer);
        ep->st.retransmits++;
        cc_on_loss(&ep->cc, now, ep->rtt.srtt);
    }
}

static void read_socket(rdt_ep *ep, int path)
{
    uint8_t buf[PKT_MAX];

    /* drain it but dont get stuck here forever. budget has to cover a
     * full window of acks or they sit in the socket and time out */
    for (int n = 0; n < TX_SLOTS; n++) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        ssize_t len = recvfrom(ep->fds[path], buf, sizeof buf, 0,
                               (struct sockaddr *)&from, &flen);
        if (len < 0)
            return; /* EAGAIN or real error, either way we're done */

        pkt_hdr h;
        const uint8_t *payload;
        if (pkt_decode(buf, len, &h, &payload) < 0) {
            ep->st.bad_pkts++;
            continue;
        }
        ep->st.pkts_recv++;

        if (h.type == PKT_DATA)
            handle_data(ep, path, &h, payload, &from);
        else if (h.type == PKT_ACK)
            handle_ack(ep, &h);
        else
            ep->st.bad_pkts++;
    }
}

int rdt_progress(rdt_ep *ep, int timeout_ms)
{
    struct pollfd pfd[RDT_MAX_PATHS];

    push_pending(ep);

    for (int i = 0; i < ep->npaths; i++) {
        pfd[i].fd = ep->fds[i];
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    int rc = poll(pfd, ep->npaths, timeout_ms);
    if (rc < 0)
        return errno == EINTR ? 0 : RDT_ERR_SYS;

    for (int i = 0; i < ep->npaths; i++)
        if (pfd[i].revents & POLLIN)
            read_socket(ep, i);

    /* timeouts AFTER reading, otherwise acks sitting in the socket
     * look like losses and we resend stuff for no reason */
    check_timeouts(ep);

    /* acks may have opened the window */
    push_pending(ep);
    return rc;
}

long rdt_recv(rdt_ep *ep, void *buf, size_t cap)
{
    if (ep->done_count == 0)
        return 0;

    done_msg *d = &ep->done[ep->done_head];
    if (d->len > cap)
        return RDT_ERR_TOOBIG;

    memcpy(buf, d->data, d->len);
    long n = d->len;
    free(d->data);
    ep->done_head = (ep->done_head + 1) % DONE_MAX;
    ep->done_count--;
    return n;
}

int rdt_idle(const rdt_ep *ep)
{
    return ep->inflight == 0 && ep->pend_count == 0;
}

void rdt_get_stats(const rdt_ep *ep, rdt_stats *st)
{
    *st = ep->st;
    st->cwnd = ep->cc.cwnd;
    st->srtt_us = ep->rtt.srtt;
    st->rto_us = ep->rtt.rto;
}
