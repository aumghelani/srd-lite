#ifndef RDT_PACKET_H
#define RDT_PACKET_H

#include <stdint.h>
#include <stddef.h>

#define PKT_MAGIC   0x5244  /* "RD" */
#define PKT_VERSION 1

#define PKT_DATA 1
#define PKT_ACK  2

/* header as it looks in memory. on the wire we pack it by hand */
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint32_t msg_id;      /* which message this belongs to */
    uint32_t psn;         /* packet seq number, unique per packet */
    uint16_t frag_idx;    /* fragment index inside the message */
    uint16_t frag_cnt;    /* total fragments */
    uint32_t msg_len;     /* full message length */
    uint16_t payload_len; /* bytes after the header */
    uint16_t src_port;    /* sender's main port, so peer can reply */
    uint32_t checksum;
} pkt_hdr;

#define PKT_HDR_SIZE 28

/* write header + payload into buf. returns total bytes or -1 */
int pkt_encode(const pkt_hdr *h, const void *payload, uint8_t *buf, size_t cap);

/* parse buf. payload points inside buf. returns 0 ok, -1 bad */
int pkt_decode(const uint8_t *buf, size_t len, pkt_hdr *h, const uint8_t **payload);

#endif
