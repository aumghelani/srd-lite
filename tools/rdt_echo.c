#define _POSIX_C_SOURCE 200809L

/* echo server. pings ('P' first byte) get sent back, anything else is
 * just counted (used for the throughput test) */

#include "rdt.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_sigint(int s) { (void)s; stop = 1; }

static void usage(void)
{
    fprintf(stderr, "usage: rdt_echo [-p port] [-P paths] [-l loss]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    rdt_config cfg;
    rdt_config_init(&cfg);
    cfg.port = 9000;

    int c;
    while ((c = getopt(argc, argv, "p:P:l:")) != -1) {
        switch (c) {
        case 'p': cfg.port = atoi(optarg); break;
        case 'P': cfg.npaths = atoi(optarg); break;
        case 'l': cfg.drop_rate = atof(optarg); break;
        default: usage();
        }
    }

    rdt_ep *ep = rdt_open(&cfg);
    if (!ep) {
        perror("rdt_open");
        return 1;
    }
    signal(SIGINT, on_sigint);
    printf("listening on %u (%d paths)\n", rdt_local_port(ep), cfg.npaths);

    static unsigned char buf[RDT_MAX_MSG];
    unsigned long long msgs = 0, bytes = 0;

    while (!stop) {
        rdt_progress(ep, 10);

        long n;
        while ((n = rdt_recv(ep, buf, sizeof buf)) > 0) {
            msgs++;
            bytes += n;
            if (buf[0] != 'P')
                continue;
            /* queue might be full, keep pumping until it fits */
            while (rdt_send(ep, buf, n) == RDT_ERR_AGAIN && !stop)
                rdt_progress(ep, 1);
        }
    }

    rdt_stats st;
    rdt_get_stats(ep, &st);
    printf("\n%llu msgs, %llu bytes, retx=%llu dups=%llu bad=%llu\n",
           msgs, bytes, (unsigned long long)st.retransmits,
           (unsigned long long)st.dups, (unsigned long long)st.bad_pkts);
    rdt_close(ep);
    return 0;
}
