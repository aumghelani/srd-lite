#include "check.h"
#include "packet.h"
#include <string.h>

static void test_roundtrip(void)
{
    uint8_t buf[256];
    const char *msg = "hello";
    pkt_hdr h = {0}, out;
    const uint8_t *pl;

    h.type = PKT_DATA;
    h.msg_id = 7;
    h.psn = 123456;
    h.frag_idx = 2;
    h.frag_cnt = 3;
    h.msg_len = 3000;
    h.payload_len = 5;
    h.src_port = 9000;

    int n = pkt_encode(&h, msg, buf, sizeof buf);
    CHECK(n == PKT_HDR_SIZE + 5);
    CHECK(pkt_decode(buf, n, &out, &pl) == 0);
    CHECK(out.type == PKT_DATA);
    CHECK(out.msg_id == 7);
    CHECK(out.psn == 123456);
    CHECK(out.frag_idx == 2 && out.frag_cnt == 3);
    CHECK(out.msg_len == 3000);
    CHECK(out.src_port == 9000);
    CHECK(memcmp(pl, "hello", 5) == 0);
    PASS();
}

static void test_short_buffer(void)
{
    uint8_t buf[10] = {0};
    pkt_hdr h;
    const uint8_t *pl;
    CHECK(pkt_decode(buf, sizeof buf, &h, &pl) == -1);
    PASS();
}

static void test_bad_magic(void)
{
    uint8_t buf[64];
    pkt_hdr h = {0}, out;
    const uint8_t *pl;
    h.type = PKT_ACK;
    int n = pkt_encode(&h, NULL, buf, sizeof buf);
    buf[0] = 0xff;
    CHECK(pkt_decode(buf, n, &out, &pl) == -1);
    PASS();
}

static void test_encode_too_small(void)
{
    uint8_t buf[30];
    char payload[100] = {0};
    pkt_hdr h = {0};
    h.payload_len = 100;
    CHECK(pkt_encode(&h, payload, buf, sizeof buf) == -1);
    PASS();
}

int main(void)
{
    test_roundtrip();
    test_short_buffer();
    test_bad_magic();
    test_encode_too_small();
    return 0;
}
