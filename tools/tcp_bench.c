#define _POSIX_C_SOURCE 200809L

/* tcp ping-pong baseline, same output as rdt_bench so they line up.
 *   server: tcp_bench -S [-p port]
 *   client: tcp_bench [-h host] [-p port] [-s size] [-n count]
 * messages are framed with a 4 byte length in front */

#include "../src/util.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_MSG (1 << 20)

/* read/write exactly n bytes, tcp can hand us partial chunks */
static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static void nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static int server(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("tcp echo on %d\n", port);

    static char buf[MAX_MSG];
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0)
            continue;
        nodelay(fd);

        uint32_t len;
        while (read_all(fd, &len, 4) == 0) {
            len = ntohl(len);
            if (len > MAX_MSG || read_all(fd, buf, len) < 0)
                break;
            uint32_t nl = htonl(len);
            if (write_all(fd, &nl, 4) < 0 || write_all(fd, buf, len) < 0)
                break;
        }
        close(fd);
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int client(const char *host, int port, size_t size, int count)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0)
        return 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        return 1;
    }
    freeaddrinfo(res);
    nodelay(fd);

    static char buf[MAX_MSG];
    uint64_t *lat = malloc(count * sizeof *lat);
    if (!lat)
        return 1;
    memset(buf, 'x', size);

    for (int i = 0; i < count; i++) {
        uint64_t t0 = now_us();
        uint32_t nl = htonl(size), rl;
        if (write_all(fd, &nl, 4) < 0 || write_all(fd, buf, size) < 0 ||
            read_all(fd, &rl, 4) < 0 || read_all(fd, buf, ntohl(rl)) < 0) {
            fprintf(stderr, "connection died\n");
            free(lat);
            return 1;
        }
        lat[i] = now_us() - t0;
    }

    qsort(lat, count, sizeof *lat, cmp_u64);
    printf("size=%zu count=%d (tcp)\n", size, count);
    printf("  p50  %8.1f us\n", (double)lat[count / 2]);
    printf("  p99  %8.1f us\n", (double)lat[count * 99 / 100]);
    printf("  p999 %8.1f us\n", (double)lat[count * 999 / 1000]);
    printf("  max  %8.1f us\n", (double)lat[count - 1]);
    free(lat);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 9001, count = 10000, is_server = 0;
    size_t size = 64;

    int c;
    while ((c = getopt(argc, argv, "Sh:p:s:n:")) != -1) {
        switch (c) {
        case 'S': is_server = 1; break;
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 's': size = strtoul(optarg, NULL, 10); break;
        case 'n': count = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: tcp_bench -S [-p port] | [-h host] [-p port] [-s size] [-n count]\n");
            return 1;
        }
    }
    if (is_server)
        return server(port);
    if (size < 1 || size > MAX_MSG || count < 1)
        return 1;
    return client(host, port, size, count);
}
