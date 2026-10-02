#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/util.h"

uint64_t
psx_now_ms(void)
{
    struct timespec ts;

    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

int
psx_set_nonblocking(int fd, bool enable)
{
    int flags;

    if((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        return -1;
    }

    if(enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    return fcntl(fd, F_SETFL, flags);
}

int
psx_set_cloexec(int fd, bool enable)
{
    int flags;

    if((flags = fcntl(fd, F_GETFD, 0)) < 0) {
        return -1;
    }

    if(enable) {
        flags |= FD_CLOEXEC;
    } else {
        flags &= ~FD_CLOEXEC;
    }

    return fcntl(fd, F_SETFD, flags);
}

bool
psx_parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long value;

    if(!s || !*s) {
        return false;
    }

    errno = 0;
    value = strtoul(s, &end, 10);
    if(errno || !end || *end || value > 0xfffful) {
        return false;
    }

    *out = (uint16_t)value;
    return true;
}

bool
psx_parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if(!s || !*s) {
        return false;
    }

    errno = 0;
    value = strtoul(s, &end, 10);
    if(errno || !end || *end || value > 0xfffffffful) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

bool
psx_parse_int(const char *s, int *out)
{
    char *end = NULL;
    long value;

    if(!s || !*s) {
        return false;
    }

    errno = 0;
    value = strtol(s, &end, 10);
    if(errno || !end || *end || value < -2147483647l || value > 2147483647l) {
        return false;
    }

    *out = (int)value;
    return true;
}

void
psx_buf_init(psx_buf_t *buf)
{
    memset(buf, 0, sizeof(*buf));
}

void
psx_buf_free(psx_buf_t *buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

int
psx_buf_append(psx_buf_t *buf, const void *data, size_t len)
{
    size_t need;

    if(len == 0) {
        return 0;
    }

    if(buf->off > 0 && buf->off == buf->len) {
        buf->len = 0;
        buf->off = 0;
    }

    need = buf->len + len;
    if(need > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 256;
        uint8_t *grown;

        while(cap < need) {
            cap *= 2;
        }

        if(!(grown = realloc(buf->data, cap))) {
            return -1;
        }

        buf->data = grown;
        buf->cap = cap;
    }

    memcpy(buf->data + buf->len, data, len);
    buf->len += len;

    return 0;
}

int
psx_buf_append_u8(psx_buf_t *buf, uint8_t value)
{
    return psx_buf_append(buf, &value, 1);
}

int
psx_buf_append_le16(psx_buf_t *buf, uint16_t value)
{
    uint8_t bytes[2] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
    };

    return psx_buf_append(buf, bytes, sizeof(bytes));
}

int
psx_buf_append_le32(psx_buf_t *buf, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)((value >> 16) & 0xff),
        (uint8_t)((value >> 24) & 0xff),
    };

    return psx_buf_append(buf, bytes, sizeof(bytes));
}

size_t
psx_buf_pending(const psx_buf_t *buf)
{
    return buf->len - buf->off;
}

ssize_t
psx_buf_flush(psx_buf_t *buf, int fd)
{
    size_t total = 0;

    while(buf->off < buf->len) {
        ssize_t n = write(fd, buf->data + buf->off, buf->len - buf->off);

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }

        buf->off += (size_t)n;
        total += (size_t)n;
    }

    if(buf->off == buf->len) {
        buf->off = 0;
        buf->len = 0;
    }

    return (ssize_t)total;
}
