#include "check.h"
#include "cc.h"

static void test_slow_start(void)
{
    cc_state c;
    cc_init(&c, 256);
    CHECK(c.cwnd == 4);
    for (int i = 0; i < 4; i++)
        cc_on_ack(&c);
    CHECK(c.cwnd == 8);
    PASS();
}

static void test_loss_halves(void)
{
    cc_state c;
    cc_init(&c, 256);
    c.cwnd = 64;
    cc_on_loss(&c, 1000000, 10000);
    CHECK(c.cwnd == 32);
    CHECK(c.ssthresh == 32);
    PASS();
}

static void test_one_cut_per_rtt(void)
{
    cc_state c;
    cc_init(&c, 256);
    c.cwnd = 64;
    cc_on_loss(&c, 1000000, 10000);
    cc_on_loss(&c, 1000500, 10000); /* same rtt, ignored */
    CHECK(c.cwnd == 32);
    cc_on_loss(&c, 1020000, 10000); /* next rtt */
    CHECK(c.cwnd == 16);
    PASS();
}

static void test_floor_and_cap(void)
{
    cc_state c;
    cc_init(&c, 10);
    for (int i = 0; i < 100; i++)
        cc_on_ack(&c);
    CHECK(c.cwnd == 10);

    for (int i = 0; i < 20; i++)
        cc_on_loss(&c, (uint64_t)(i + 1) * 1000000, 1000);
    CHECK(c.cwnd == 2);
    PASS();
}

static void test_can_send(void)
{
    cc_state c;
    cc_init(&c, 256);
    CHECK(cc_can_send(&c, 3));
    CHECK(!cc_can_send(&c, 4));
    PASS();
}

int main(void)
{
    test_slow_start();
    test_loss_halves();
    test_one_cut_per_rtt();
    test_floor_and_cap();
    test_can_send();
    return 0;
}
