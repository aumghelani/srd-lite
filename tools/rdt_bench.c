#define _POSIX_C_SOURCE 200809L

/* benchmark client for rdt_echo
 *   ping mode:   send one msg, wait for echo, repeat. prints latency
 *   stream mode: blast msgs, wait until all acked. prints throughput */

#include "rdt.h"
#include "../src/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: rdt_bench [-h host] [-p port] [-m ping|stream] [-s size]\n"
        "                 [-n count] [-P paths] [-l loss]\n");
    exit(1);
}

static int ping(rdt_ep *ep, size_t size, int count)
{
    static unsigned char buf[RDT_MAX_MSG], rbuf[RDT_MAX_MSG];
    uint64_t *lat = malloc(count * sizeof *lat);
    if (!lat)
        return 1;
    memset(buf, 'x', size);
    buf[0] = 'P';

    for (int i = 0; i < count; i++) {
        uint64_t t0 = now_us();
        rdt_send(ep, buf, size);

        long n = 0;
        while (n == 0) {
            rdt_progress(ep, 1);
            n = rdt_recv(ep, rbuf, sizeof rbuf);
            if (now_us() - t0 > 5000000) {
                fprintf(stderr, "timed out waiting for echo %d\n", i);
                free(lat);
                return 1;
            }
        }
        lat[i] = now_us() - t0;
    }

    qsort(lat, count, sizeof *lat, cmp_u64);
    printf("size=%zu count=%d\n", size, count);
    printf("  p50  %8.1f us\n", (double)lat[count / 2]);
    printf("  p99  %8.1f us\n", (double)lat[count * 99 / 100]);
    printf("  p999 %8.1f us\n", (double)lat[count * 999 / 1000]);
    printf("  max  %8.1f us\n", (double)lat[count - 1]);
    free(lat);
    return 0;
}

static int stream(rdt_ep *ep, size_t size, int count)
{
    static unsigned char buf[RDT_MAX_MSG];
    memset(buf, 'x', size);
    buf[0] = 'S';

    uint64_t t0 = now_us();
    int sent = 0;
    while (sent < count || !rdt_idle(ep)) {
        while (sent < count && rdt_send(ep, buf, size) == 0)
            sent++;
        rdt_progress(ep, 0);
    }
    double secs = (now_us() - t0) / 1e6;
    double mb = (double)size * count / 1e6;

    printf("size=%zu count=%d\n", size, count);
    printf("  %.2f MB in %.3f s = %.1f MB/s, %.0f msgs/s\n",
           mb, secs, mb / secs, count / secs);
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 9000, count = 10000, streaming = 0;
    size_t size = 64;
    rdt_config cfg;
    rdt_config_init(&cfg);

    int c;
    while ((c = getopt(argc, argv, "h:p:m:s:n:P:l:")) != -1) {
        switch (c) {
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'm': streaming = strcmp(optarg, "stream") == 0; break;
        case 's': size = strtoul(optarg, NULL, 10); break;
        case 'n': count = atoi(optarg); break;
        case 'P': cfg.npaths = atoi(optarg); break;
        case 'l': cfg.drop_rate = atof(optarg); break;
        default: usage();
        }
    }
    if (size < 1 || size > RDT_MAX_MSG || count < 1)
        usage();

    rdt_ep *ep = rdt_open(&cfg);
    if (!ep || rdt_set_peer(ep, host, port) < 0) {
        fprintf(stderr, "cant set up endpoint\n");
        return 1;
    }

    int rc = streaming ? stream(ep, size, count) : ping(ep, size, count);

    rdt_stats st;
    rdt_get_stats(ep, &st);
    printf("  retx=%llu fast=%llu srtt=%lluus cwnd=%.1f\n",
           (unsigned long long)st.retransmits, (unsigned long long)st.fast_retx,
           (unsigned long long)st.srtt_us, st.cwnd);
    rdt_close(ep);
    return rc;
}
