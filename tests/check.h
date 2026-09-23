#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* tiny test helper. bail out on first failure */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define PASS() printf("  ok  %s\n", __func__)

#endif
