#ifndef RDT_CC_H
#define RDT_CC_H

#include <stdint.h>

/* simple aimd congestion control, window counted in packets */
typedef struct {
    double cwnd;
    double ssthresh;
    double max_cwnd;
    uint64_t last_cut_us; /* only cut once per rtt */
} cc_state;

void cc_init(cc_state *c, double max_cwnd);
void cc_on_ack(cc_state *c);
void cc_on_loss(cc_state *c, uint64_t now, uint64_t srtt);
int  cc_can_send(const cc_state *c, int inflight);

#endif
