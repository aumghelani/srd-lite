#include "cc.h"

void cc_init(cc_state *c, double max_cwnd)
{
    c->cwnd = 4;
    c->ssthresh = max_cwnd;
    c->max_cwnd = max_cwnd;
    c->last_cut_us = 0;
}

void cc_on_ack(cc_state *c)
{
    if (c->cwnd < c->ssthresh)
        c->cwnd += 1;             /* slow start: doubles every rtt */
    else
        c->cwnd += 1.0 / c->cwnd; /* congestion avoidance: +1 per rtt */

    if (c->cwnd > c->max_cwnd)
        c->cwnd = c->max_cwnd;
}

void cc_on_loss(cc_state *c, uint64_t now, uint64_t srtt)
{
    /* a burst of losses from the same window should count once */
    if (now - c->last_cut_us < srtt)
        return;

    c->ssthresh = c->cwnd / 2;
    if (c->ssthresh < 2)
        c->ssthresh = 2;
    c->cwnd = c->ssthresh;
    c->last_cut_us = now;
}

int cc_can_send(const cc_state *c, int inflight)
{
    return inflight < (int)c->cwnd;
}
