#include "check.h"
#include "rtt.h"

static void test_initial(void)
{
    rtt_est r;
    rtt_init(&r);
    CHECK(r.rto == RTO_INITIAL);
    CHECK(!r.have_sample);
    PASS();
}

static void test_first_sample(void)
{
    rtt_est r;
    rtt_init(&r);
    rtt_sample(&r, 20000);
    /* srtt = 20ms, rttvar = 10ms, rto = 20 + 40 = 60ms */
    CHECK(r.srtt == 20000);
    CHECK(r.rto == 60000);
    PASS();
}

static void test_clamps(void)
{
    rtt_est r;
    rtt_init(&r);
    for (int i = 0; i < 50; i++)
        rtt_sample(&r, 10); /* super tiny rtt */
    CHECK(r.rto == RTO_MIN);

    rtt_init(&r);
    rtt_sample(&r, 10000000); /* 10s */
    CHECK(r.rto == RTO_MAX);
    PASS();
}

static void test_converges(void)
{
    rtt_est r;
    rtt_init(&r);
    for (int i = 0; i < 100; i++)
        rtt_sample(&r, 30000);
    /* steady rtt -> srtt goes to 30ms and variance shrinks */
    CHECK(r.srtt > 29900 && r.srtt < 30100);
    CHECK(r.rttvar < 100);
    PASS();
}

int main(void)
{
    test_initial();
    test_first_sample();
    test_clamps();
    test_converges();
    return 0;
}
