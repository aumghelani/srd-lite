#include "check.h"
#include "util.h"

static void test_psn_before(void)
{
    CHECK(psn_before(1, 2));
    CHECK(!psn_before(2, 1));
    CHECK(!psn_before(5, 5));
    /* wrap: 0xfffffff0 comes before 3 */
    CHECK(psn_before(0xfffffff0u, 3));
    CHECK(!psn_before(3, 0xfffffff0u));
    PASS();
}

static void test_rng_not_stuck(void)
{
    uint32_t s = 1;
    uint32_t a = xorshift32(&s);
    uint32_t b = xorshift32(&s);
    CHECK(a != 0 && b != 0 && a != b);
    PASS();
}

static void test_clock_moves(void)
{
    uint64_t a = now_us();
    volatile int x = 0;
    for (int i = 0; i < 1000000; i++) x += i;
    uint64_t b = now_us();
    CHECK(b >= a);
    PASS();
}

int main(void)
{
    test_psn_before();
    test_rng_not_stuck();
    test_clock_moves();
    return 0;
}
