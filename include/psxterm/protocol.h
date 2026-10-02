#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psxterm/util.h"

/*
 * PTTY/1 - PSXTerm terminal protocol, version 1.
 *
 * All integers on the wire are little-endian. The header is a fixed
 * 16-byte frame:
 *
 *   offset  size  field
 *   0       4     magic           (PTTY_MAGIC)
 *   4       1     version         (PTTY_VERSION)
 *   5       1     message type    (ptty_msg_type_t)
 *   6       2     flags
 *   8       4     session id      (0 before OPEN_OK)
 *   12      4     payload length  (<= PTTY_MAX_PAYLOAD)
 *
 * Headers are never cast directly over received buffers; they are encoded
 * and decoded field by field with explicit bounds checks.
 */

/* Magic value whose little-endian wire bytes spell 'P', 'T', 'T', 'Y'. */
#define PTTY_MAGIC 0x59545450u
#define PTTY_VERSION 1
#define PTTY_HEADER_SIZE 16
#define PTTY_MAX_PAYLOAD 65536u
#define PTTY_MAX_FRAME (PTTY_HEADER_SIZE + PTTY_MAX_PAYLOAD)

typedef enum {
    PTTY_MSG_HELLO = 1,
    PTTY_MSG_HELLO_ACK = 2,
    PTTY_MSG_OPEN = 3,
    PTTY_MSG_OPEN_OK = 4,
    PTTY_MSG_CLOSE = 5,
    /* STDIN carries raw keyboard bytes. A zero-length STDIN payload is an
     * end-of-file marker for the foreground process. */
    PTTY_MSG_STDIN = 6,
    PTTY_MSG_STDOUT = 7,
    PTTY_MSG_STDERR = 8,
    PTTY_MSG_RESIZE = 9,
    PTTY_MSG_SIGNAL = 10,
    PTTY_MSG_EXEC = 11,
    PTTY_MSG_EXIT = 12,
    PTTY_MSG_PING = 13,
    PTTY_MSG_PONG = 14,
    /*
     * Diagnostics: the client requests a report, the server streams it as
     * DIAG_DATA frames (chunked when necessary) and finishes with a DIAG_DONE
     * frame carrying the overall status.
     */
    PTTY_MSG_DIAG_REQUEST = 15,
    PTTY_MSG_DIAG_DATA = 16,
    PTTY_MSG_DIAG_DONE = 17,
    /*
     * Capability advertisement, sent by the server right after OPEN_OK.
     * Clients that do not know the frame ignore it; clients talking to an
     * older daemon simply never receive it and must assume the baseline
     * PTTY/1 feature set instead of guessing from version strings.
     */
    PTTY_MSG_CAPS = 18
} ptty_msg_type_t;

/* DIAG_REQUEST flags */
#define PTTY_DIAG_FLAG_JSON 0x01u

/*
 * Capability bitmask carried by PTTY_MSG_CAPS as a little-endian uint32.
 * A capability is advertised only when the running system can actually
 * provide it (for example EXEC is absent when the process backend is
 * unavailable, and JOB_CONTROL is absent on the PipeTTY fallback).
 */
#define PTTY_CAP_REAL_PTY 0x00000001u
#define PTTY_CAP_PIPE_TTY 0x00000002u
#define PTTY_CAP_EXEC 0x00000004u
#define PTTY_CAP_FILE_TRANSFER 0x00000008u
#define PTTY_CAP_SESSION_RESUME 0x00000010u
#define PTTY_CAP_JOB_CONTROL 0x00000020u
#define PTTY_CAP_AUTH_CHALLENGE 0x00000040u
#define PTTY_CAP_COMPRESSION 0x00000080u
#define PTTY_CAP_JSON_DIAGNOSTICS 0x00000100u

/* Status byte of HELLO_ACK. */
enum {
    PTTY_ACK_OK = 0,
    PTTY_ACK_AUTH_REQUIRED = 1,
    PTTY_ACK_AUTH_FAILED = 2,
    PTTY_ACK_SERVER_BUSY = 3
};

/* Kind byte of an EXIT payload. */
enum {
    PTTY_EXIT_PROCESS = 0, /* a foreground process finished */
    PTTY_EXIT_SHELL = 1    /* the shell itself exited */
};

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint32_t payload_length;
} ptty_header_t;

/* Returns 0 on success, -1 if the header cannot be represented on the wire. */
int ptty_header_encode(const ptty_header_t *header, uint8_t out[PTTY_HEADER_SIZE]);

/* Validates magic, version and payload length. Returns 0 or -1. */
int ptty_header_decode(const uint8_t in[PTTY_HEADER_SIZE], ptty_header_t *header);

/* Blocking write of a full frame; loops over partial writes. 0 or -1. */
int ptty_send_frame(int fd, const ptty_header_t *header, const void *payload);

int ptty_send_simple(int fd, uint8_t type, uint16_t flags, uint32_t session_id,
                     const void *payload, uint32_t payload_len);

/* Append a complete framed message to an output queue. 0 or -1. */
int ptty_queue_frame(psx_buf_t *buf, const ptty_header_t *header, const void *payload);

/* HELLO payload: [u8 name_len][name][u8 token_len][token] */
size_t ptty_hello_encode(uint8_t *out, size_t out_cap, const char *name,
                         const char *token);
int ptty_hello_decode(const uint8_t *payload, size_t payload_len, char *name,
                      size_t name_cap, char *token, size_t token_cap);

/* RESIZE payload: [u16 rows][u16 cols] */
size_t ptty_resize_encode(uint8_t out[4], uint16_t rows, uint16_t cols);
int ptty_resize_decode(const uint8_t in[4], uint16_t *rows, uint16_t *cols);

/*
 * Incremental frame reader. Works on non-blocking descriptors: a call may
 * return PTTY_READ_AGAIN when a partial header or payload is pending; state
 * is preserved across calls.
 */
typedef enum {
    PTTY_READ_OK = 0,
    PTTY_READ_AGAIN,
    PTTY_READ_EOF,
    PTTY_READ_ERROR,
    PTTY_READ_PROTOCOL
} ptty_read_result_t;

typedef struct {
    int fd;
    uint8_t header_buf[PTTY_HEADER_SIZE];
    size_t header_len;
    bool header_ready;
    ptty_header_t header;
    uint8_t *payload;
    size_t payload_cap;
    size_t payload_read;
} ptty_reader_t;

void ptty_reader_init(ptty_reader_t *reader, int fd);
void ptty_reader_reset(ptty_reader_t *reader);
void ptty_reader_destroy(ptty_reader_t *reader);

/*
 * Reads at most one frame. On PTTY_READ_OK, *header_out and *payload_out are
 * valid until the next call. payload_out is NULL for empty payloads.
 */
ptty_read_result_t ptty_read_frame(ptty_reader_t *reader, ptty_header_t *header_out,
                                   const uint8_t **payload_out);

const char *ptty_msg_name(uint8_t type);
