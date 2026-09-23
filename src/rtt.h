#ifndef RDT_RTT_H
#define RDT_RTT_H

#include <stdint.h>

/* rto limits in us */
#define RTO_MIN     5000      /* 5ms, loopback is fast */
#define RTO_MAX     2000000   /* 2s */
#define RTO_INITIAL 100000    /* 100ms before we have any sample */

typedef struct {
    double srtt;    /* smoothed rtt */
    double rttvar;  /* rtt variance */
    uint64_t rto;   /* current timeout */
    int have_sample;
} rtt_est;

void rtt_init(rtt_est *r);
void rtt_sample(rtt_est *r, uint64_t rtt_us);

#endif
