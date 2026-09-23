#include "check.h"
#include "rdt.h"
#include "../src/util.h"
#include <string.h>

/* message i = [4 byte index][bytes derived from i] so we can verify it */
static size_t make_msg(uint8_t *buf, uint32_t i, size_t len)
{
    memcpy(buf, &i, 4);
    for (size_t k = 4; k < len; k++)
        buf[k] = (uint8_t)(i * 31 + k);
    return len;
}

static int check_msg(const uint8_t *buf, size_t len, uint32_t *idx)
{
    memcpy(idx, buf, 4);
    for (size_t k = 4; k < len; k++)
        if (buf[k] != (uint8_t)(*idx * 31 + k))
            return 0;
    return 1;
}

static void run(int nmsgs, int npaths, double drop, size_t max_len)
{
    rdt_config ca, cb;
    rdt_config_init(&ca);
    rdt_config_init(&cb);
    ca.npaths = npaths;
    cb.npaths = npaths;
    ca.drop_rate = drop;
    cb.drop_rate = drop;
    cb.seed = 999;

    rdt_ep *a = rdt_open(&ca);
    rdt_ep *b = rdt_open(&cb);
    CHECK(a && b);
    CHECK(rdt_set_peer(a, "127.0.0.1", rdt_local_port(b)) == 0);

    static uint8_t buf[RDT_MAX_MSG];
    static uint8_t got[10000];
    size_t lens[10000];
    uint32_t rng = 42;
    memset(got, 0, sizeof got);

    int sent = 0, recvd = 0;
    uint64_t start = now_us();

    while (recvd < nmsgs) {
        /* send as many as the queue will take */
        while (sent < nmsgs) {
            size_t len = 4 + xorshift32(&rng) % (max_len - 4);
            make_msg(buf, sent, len);
            int rc = rdt_send(a, buf, len);
            if (rc == RDT_ERR_AGAIN)
                break;
            CHECK(rc == 0);
            lens[sent] = len;
            sent++;
        }

        rdt_progress(a, 0);
        rdt_progress(b, 1);

        long n;
        while ((n = rdt_recv(b, buf, sizeof buf)) > 0) {
            uint32_t idx;
            CHECK(check_msg(buf, n, &idx));
            CHECK(idx < (uint32_t)nmsgs);
            CHECK((size_t)n == lens[idx]);
            CHECK(!got[idx]); /* exactly once */
            got[idx] = 1;
            recvd++;
        }

        CHECK(now_us() - start < 60 * 1000000ULL); /* dont hang ci */
    }

    /* let the last acks land */
    while (!rdt_idle(a) && now_us() - start < 60 * 1000000ULL) {
        rdt_progress(a, 1);
        rdt_progress(b, 0);
    }
    CHECK(rdt_idle(a));

    rdt_stats sa, sb;
    rdt_get_stats(a, &sa);
    rdt_get_stats(b, &sb);
    printf("  %d msgs, %d paths, %.0f%% loss: %.1f ms, retx=%llu dups=%llu cwnd=%.1f\n",
           nmsgs, npaths, drop * 100, (now_us() - start) / 1000.0,
           (unsigned long long)sa.retransmits, (unsigned long long)sb.dups, sa.cwnd);
    CHECK(sa.give_ups == 0);

    rdt_close(a);
    rdt_close(b);
}

static void test_no_loss(void)    { run(2000, 1, 0.0, 4000);  PASS(); }
static void test_multipath(void)  { run(2000, 4, 0.0, 4000);  PASS(); }
static void test_loss_5(void)     { run(2000, 4, 0.05, 4000); PASS(); }
static void test_loss_20(void)    { run(500, 4, 0.20, 4000);  PASS(); }
static void test_big_msgs(void)   { run(50, 4, 0.02, 300000); PASS(); }

int main(void)
{
    test_no_loss();
    test_multipath();
    test_loss_5();
    test_loss_20();
    test_big_msgs();
    return 0;
}
