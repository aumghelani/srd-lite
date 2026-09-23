#include "packet.h"
#include <string.h>

/* big endian helpers. doing it by hand so struct padding doesnt matter */
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* fnv-1a over the whole packet. checksum field must be zero while hashing */
static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int pkt_encode(const pkt_hdr *h, const void *payload, uint8_t *buf, size_t cap)
{
    size_t total = PKT_HDR_SIZE + h->payload_len;
    if (total > cap)
        return -1;

    put16(buf + 0, PKT_MAGIC);
    buf[2] = PKT_VERSION;
    buf[3] = h->type;
    put32(buf + 4, h->msg_id);
    put32(buf + 8, h->psn);
    put16(buf + 12, h->frag_idx);
    put16(buf + 14, h->frag_cnt);
    put32(buf + 16, h->msg_len);
    put16(buf + 20, h->payload_len);
    put16(buf + 22, h->src_port);
    put32(buf + 24, 0);

    if (h->payload_len)
        memcpy(buf + PKT_HDR_SIZE, payload, h->payload_len);

    put32(buf + 24, fnv1a(buf, total));
    return total;
}

int pkt_decode(const uint8_t *buf, size_t len, pkt_hdr *h, const uint8_t **payload)
{
    if (len < PKT_HDR_SIZE)
        return -1;

    h->magic = get16(buf);
    h->version = buf[2];
    h->type = buf[3];
    h->msg_id = get32(buf + 4);
    h->psn = get32(buf + 8);
    h->frag_idx = get16(buf + 12);
    h->frag_cnt = get16(buf + 14);
    h->msg_len = get32(buf + 16);
    h->payload_len = get16(buf + 20);
    h->src_port = get16(buf + 22);
    h->checksum = get32(buf + 24);

    if (h->magic != PKT_MAGIC || h->version != PKT_VERSION)
        return -1;
    if (PKT_HDR_SIZE + h->payload_len > len)
        return -1;

    /* zero the checksum field in a copy of the header and re-hash */
    uint8_t tmp[PKT_HDR_SIZE];
    memcpy(tmp, buf, PKT_HDR_SIZE);
    put32(tmp + 24, 0);
    uint32_t sum = fnv1a(tmp, PKT_HDR_SIZE);
    if (sum != h->checksum)
        return -1;

    *payload = buf + PKT_HDR_SIZE;
    return 0;
}
