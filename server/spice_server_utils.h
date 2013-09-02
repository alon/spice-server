#ifndef H_SPICE_SERVER_UTIL
#define H_SPICE_SERVER_UTIL

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include "common/log.h"

static inline void set_bit(int index, uint32_t *addr)
{
    uint32_t mask = 1 << index;
    __sync_or_and_fetch(addr, mask);
}

static inline void clear_bit(int index, uint32_t *addr)
{
    uint32_t mask = ~(1 << index);
    __sync_and_and_fetch(addr, mask);
}

static inline int test_bit(int index, uint32_t val)
{
    return val & (1u << index);
}

static inline void xwrite(int fd, void *in_buf, int n)
{
    uint8_t *buf = in_buf;
    do {
        int now;
        if ((now = write(fd, buf, n)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            spice_error("%s", strerror(errno));
        }
        buf += now;
        n -= now;
    } while (n);
}

static inline void xread(int fd, void *in_buf, int n)
{
    uint8_t *buf = in_buf;
    do {
        int now;
        if ((now = read(fd, buf, n)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            spice_error("%s", strerror(errno));
        }
        buf += now;
        n -= now;
    } while (n);
}

#endif
