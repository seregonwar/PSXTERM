#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Monotonic milliseconds. */
uint64_t psx_now_ms(void);

/* Best-effort helpers to put a descriptor into non-blocking/cloexec mode. */
int psx_set_nonblocking(int fd, bool enable);
int psx_set_cloexec(int fd, bool enable);

bool psx_parse_u16(const char *s, uint16_t *out);
bool psx_parse_u32(const char *s, uint32_t *out);
bool psx_parse_int(const char *s, int *out);

/*
 * Growable byte buffer used for both framed output queues and parsing
 * scratch space. The buffer keeps a read offset so partially flushed data
 * does not need to be memmoved on every write.
 */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t off;
} psx_buf_t;

void psx_buf_init(psx_buf_t *buf);
void psx_buf_free(psx_buf_t *buf);

int psx_buf_append(psx_buf_t *buf, const void *data, size_t len);
int psx_buf_append_u8(psx_buf_t *buf, uint8_t value);
int psx_buf_append_le16(psx_buf_t *buf, uint16_t value);
int psx_buf_append_le32(psx_buf_t *buf, uint32_t value);

/* Number of pending (unflushed) bytes. */
size_t psx_buf_pending(const psx_buf_t *buf);

/*
 * Flush as much of the pending data as possible to fd.
 * Returns the number of bytes written (>= 0), or -1 on a real error.
 * A return of 0 with pending data means the descriptor is not writable now.
 */
ssize_t psx_buf_flush(psx_buf_t *buf, int fd);
