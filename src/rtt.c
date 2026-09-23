#include "rtt.h"

/* same idea as tcp (rfc 6298) */
void rtt_init(rtt_est *r)
{
    r->srtt = 0;
    r->rttvar = 0;
    r->rto = RTO_INITIAL;
    r->have_sample = 0;
}

void rtt_sample(rtt_est *r, uint64_t rtt_us)
{
    double m = (double)rtt_us;

    if (!r->have_sample) {
        r->srtt = m;
        r->rttvar = m / 2;
        r->have_sample = 1;
    } else {
        double diff = r->srtt - m;
        if (diff < 0) diff = -diff;
        r->rttvar = 0.75 * r->rttvar + 0.25 * diff;
        r->srtt = 0.875 * r->srtt + 0.125 * m;
    }

    r->rto = r->srtt + 4 * r->rttvar;
    if (r->rto < RTO_MIN) r->rto = RTO_MIN;
    if (r->rto > RTO_MAX) r->rto = RTO_MAX;
}
