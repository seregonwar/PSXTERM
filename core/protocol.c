#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psxterm/protocol.h"

static void
put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xff);
    out[1] = (uint8_t)((value >> 8) & 0xff);
}

static void
put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xff);
    out[1] = (uint8_t)((value >> 8) & 0xff);
    out[2] = (uint8_t)((value >> 16) & 0xff);
    out[3] = (uint8_t)((value >> 24) & 0xff);
}

static uint16_t
get_le16(const uint8_t *in)
{
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t
get_le32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

int
ptty_header_encode(const ptty_header_t *header, uint8_t out[PTTY_HEADER_SIZE])
{
    if(!header || header->payload_length > PTTY_MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }

    put_le32(out + 0, header->magic);
    out[4] = header->version;
    out[5] = header->type;
    put_le16(out + 6, header->flags);
    put_le32(out + 8, header->session_id);
    put_le32(out + 12, header->payload_length);

    return 0;
}

int
ptty_header_decode(const uint8_t in[PTTY_HEADER_SIZE], ptty_header_t *header)
{
    uint32_t magic = get_le32(in + 0);

    if(magic != PTTY_MAGIC) {
        errno = EBADMSG;
        return -1;
    }

    if(in[4] != PTTY_VERSION) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    header->magic = magic;
    header->version = in[4];
    header->type = in[5];
    header->flags = get_le16(in + 6);
    header->session_id = get_le32(in + 8);
    header->payload_length = get_le32(in + 12);

    if(header->payload_length > PTTY_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }

    return 0;
}

static int
write_all(int fd, const uint8_t *data, size_t len)
{
    size_t written = 0;

    while(written < len) {
        ssize_t n = write(fd, data + written, len - written);

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                int rc = poll(&pfd, 1, 5000);

                if(rc < 0 && errno != EINTR) {
                    return -1;
                }
                continue;
            }
            return -1;
        }

        written += (size_t)n;
    }

    return 0;
}

int
ptty_send_frame(int fd, const ptty_header_t *header, const void *payload)
{
    uint8_t raw[PTTY_HEADER_SIZE];

    if(ptty_header_encode(header, raw) < 0) {
        return -1;
    }

    if(write_all(fd, raw, sizeof(raw)) < 0) {
        return -1;
    }

    if(header->payload_length > 0) {
        if(!payload) {
            errno = EINVAL;
            return -1;
        }
        if(write_all(fd, payload, header->payload_length) < 0) {
            return -1;
        }
    }

    return 0;
}

int
ptty_send_simple(int fd, uint8_t type, uint16_t flags, uint32_t session_id,
                 const void *payload, uint32_t payload_len)
{
    ptty_header_t header = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = type,
        .flags = flags,
        .session_id = session_id,
        .payload_length = payload_len,
    };

    return ptty_send_frame(fd, &header, payload);
}

int
ptty_queue_frame(psx_buf_t *buf, const ptty_header_t *header, const void *payload)
{
    uint8_t raw[PTTY_HEADER_SIZE];

    if(ptty_header_encode(header, raw) < 0) {
        return -1;
    }

    if(psx_buf_append(buf, raw, sizeof(raw)) < 0) {
        return -1;
    }

    if(header->payload_length > 0 && payload) {
        if(psx_buf_append(buf, payload, header->payload_length) < 0) {
            return -1;
        }
    }

    return 0;
}

size_t
ptty_hello_encode(uint8_t *out, size_t out_cap, const char *name, const char *token)
{
    size_t name_len = name ? strlen(name) : 0;
    size_t token_len = token ? strlen(token) : 0;
    size_t total;

    if(name_len > 255 || token_len > 255) {
        return 0;
    }

    total = 2 + name_len + token_len;
    if(total > out_cap) {
        return 0;
    }

    out[0] = (uint8_t)name_len;
    if(name_len) {
        memcpy(out + 1, name, name_len);
    }

    out[1 + name_len] = (uint8_t)token_len;
    if(token_len) {
        memcpy(out + 2 + name_len, token, token_len);
    }

    return total;
}

int
ptty_hello_decode(const uint8_t *payload, size_t payload_len, char *name,
                  size_t name_cap, char *token, size_t token_cap)
{
    size_t name_len;
    size_t token_len;

    if(!payload || payload_len < 2) {
        return -1;
    }

    name_len = payload[0];
    if(payload_len < 1 + name_len + 1) {
        return -1;
    }

    token_len = payload[1 + name_len];
    if(payload_len != 1 + name_len + 1 + token_len) {
        return -1;
    }

    if(name_len + 1 > name_cap || token_len + 1 > token_cap) {
        return -1;
    }

    memcpy(name, payload + 1, name_len);
    name[name_len] = '\0';

    memcpy(token, payload + 2 + name_len, token_len);
    token[token_len] = '\0';

    return 0;
}

size_t
ptty_resize_encode(uint8_t out[4], uint16_t rows, uint16_t cols)
{
    put_le16(out + 0, rows);
    put_le16(out + 2, cols);

    return 4;
}

int
ptty_resize_decode(const uint8_t in[4], uint16_t *rows, uint16_t *cols)
{
    if(!in) {
        return -1;
    }

    *rows = get_le16(in + 0);
    *cols = get_le16(in + 2);

    if(*rows == 0 || *cols == 0) {
        return -1;
    }

    return 0;
}

void
ptty_reader_init(ptty_reader_t *reader, int fd)
{
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
}

void
ptty_reader_reset(ptty_reader_t *reader)
{
    reader->header_len = 0;
    reader->header_ready = false;
    reader->payload_read = 0;
    memset(&reader->header, 0, sizeof(reader->header));
}

void
ptty_reader_destroy(ptty_reader_t *reader)
{
    free(reader->payload);
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
}

static ptty_read_result_t
read_exact(ptty_reader_t *reader, uint8_t *dst, size_t len, size_t *done)
{
    while(*done < len) {
        ssize_t n = read(reader->fd, dst + *done, len - *done);

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return PTTY_READ_AGAIN;
            }
            return PTTY_READ_ERROR;
        }

        if(n == 0) {
            return PTTY_READ_EOF;
        }

        *done += (size_t)n;
    }

    return PTTY_READ_OK;
}

ptty_read_result_t
ptty_read_frame(ptty_reader_t *reader, ptty_header_t *header_out,
                const uint8_t **payload_out)
{
    ptty_read_result_t rc;

    if(payload_out) {
        *payload_out = NULL;
    }

    if(!reader->header_ready) {
        rc = read_exact(reader, reader->header_buf, PTTY_HEADER_SIZE,
                        &reader->header_len);
        if(rc != PTTY_READ_OK) {
            return rc;
        }

        if(ptty_header_decode(reader->header_buf, &reader->header) < 0) {
            ptty_reader_reset(reader);
            return PTTY_READ_PROTOCOL;
        }

        reader->header_ready = true;
        reader->payload_read = 0;
    }

    if(reader->header.payload_length > 0) {
        if(reader->payload_cap < reader->header.payload_length) {
            uint8_t *grown = realloc(reader->payload, reader->header.payload_length);

            if(!grown) {
                return PTTY_READ_ERROR;
            }

            reader->payload = grown;
            reader->payload_cap = reader->header.payload_length;
        }

        rc = read_exact(reader, reader->payload, reader->header.payload_length,
                        &reader->payload_read);
        if(rc != PTTY_READ_OK) {
            return rc;
        }
    }

    *header_out = reader->header;
    if(payload_out && reader->header.payload_length > 0) {
        *payload_out = reader->payload;
    }

    ptty_reader_reset(reader);

    return PTTY_READ_OK;
}

const char *
ptty_msg_name(uint8_t type)
{
    switch(type) {
    case PTTY_MSG_HELLO: return "HELLO";
    case PTTY_MSG_HELLO_ACK: return "HELLO_ACK";
    case PTTY_MSG_OPEN: return "OPEN";
    case PTTY_MSG_OPEN_OK: return "OPEN_OK";
    case PTTY_MSG_CLOSE: return "CLOSE";
    case PTTY_MSG_STDIN: return "STDIN";
    case PTTY_MSG_STDOUT: return "STDOUT";
    case PTTY_MSG_STDERR: return "STDERR";
    case PTTY_MSG_RESIZE: return "RESIZE";
    case PTTY_MSG_SIGNAL: return "SIGNAL";
    case PTTY_MSG_EXEC: return "EXEC";
    case PTTY_MSG_EXIT: return "EXIT";
    case PTTY_MSG_PING: return "PING";
    case PTTY_MSG_PONG: return "PONG";
    case PTTY_MSG_DIAG_REQUEST: return "DIAG_REQUEST";
    case PTTY_MSG_DIAG_DATA: return "DIAG_DATA";
    case PTTY_MSG_DIAG_DONE: return "DIAG_DONE";
    case PTTY_MSG_CAPS: return "CAPS";
    case PTTY_MSG_DETACH: return "DETACH";
    case PTTY_MSG_ATTACH: return "ATTACH";
    case PTTY_MSG_ATTACH_OK: return "ATTACH_OK";
    case PTTY_MSG_ATTACH_FAIL: return "ATTACH_FAIL";
    case PTTY_MSG_SESSION_INFO: return "SESSION_INFO";
    case PTTY_MSG_SESSIONS_REQUEST: return "SESSIONS_REQUEST";
    case PTTY_MSG_SESSIONS_DATA: return "SESSIONS_DATA";
    case PTTY_MSG_SESSIONS_DONE: return "SESSIONS_DONE";
    default: return "UNKNOWN";
    }
}
