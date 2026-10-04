#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "psxterm/protocol.h"
#include "psxterm/util.h"

#include "psx_test.h"

static void
test_header_roundtrip(void)
{
    ptty_header_t in = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_STDIN,
        .flags = 0x1234,
        .session_id = 0xdeadbeef,
        .payload_length = 42,
    };
    ptty_header_t out;
    uint8_t raw[PTTY_HEADER_SIZE];

    PSX_CHECK_EQ(ptty_header_encode(&in, raw), 0);
    PSX_CHECK_EQ(ptty_header_decode(raw, &out), 0);
    PSX_CHECK_EQ(out.magic, PTTY_MAGIC);
    PSX_CHECK_EQ(out.version, PTTY_VERSION);
    PSX_CHECK_EQ(out.type, PTTY_MSG_STDIN);
    PSX_CHECK_EQ(out.flags, 0x1234);
    PSX_CHECK_EQ(out.session_id, 0xdeadbeef);
    PSX_CHECK_EQ(out.payload_length, 42);

    /* little-endian on the wire */
    PSX_CHECK_EQ(raw[0], 'P');
    PSX_CHECK_EQ(raw[1], 'T');
    PSX_CHECK_EQ(raw[2], 'T');
    PSX_CHECK_EQ(raw[3], 'Y');
    PSX_CHECK_EQ(raw[4], 1);
    PSX_CHECK_EQ(raw[5], PTTY_MSG_STDIN);
    PSX_CHECK_EQ(raw[6], 0x34);
    PSX_CHECK_EQ(raw[7], 0x12);
    PSX_CHECK_EQ(raw[12], 42);
}

static void
test_header_rejection(void)
{
    ptty_header_t header;
    uint8_t raw[PTTY_HEADER_SIZE];

    ptty_header_t valid = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_PING,
        .flags = 0,
        .session_id = 0,
        .payload_length = 0,
    };

    PSX_CHECK_EQ(ptty_header_encode(&valid, raw), 0);

    {
        uint8_t bad[PTTY_HEADER_SIZE];
        memcpy(bad, raw, sizeof(bad));
        bad[0] = 'X';
        PSX_CHECK_EQ(ptty_header_decode(bad, &header), -1);
    }

    {
        uint8_t bad[PTTY_HEADER_SIZE];
        memcpy(bad, raw, sizeof(bad));
        bad[4] = 99;
        PSX_CHECK_EQ(ptty_header_decode(bad, &header), -1);
    }

    {
        uint8_t bad[PTTY_HEADER_SIZE];
        memcpy(bad, raw, sizeof(bad));
        /* payload_length = PTTY_MAX_PAYLOAD + 1 (little-endian) */
        bad[12] = 0x01;
        bad[13] = 0x00;
        bad[14] = 0x01;
        bad[15] = 0x00;
        PSX_CHECK_EQ(ptty_header_decode(bad, &header), -1);
    }

    /* encoding refuses oversized payloads */
    valid.payload_length = PTTY_MAX_PAYLOAD + 1;
    PSX_CHECK_EQ(ptty_header_encode(&valid, raw), -1);
}

static void
test_hello_payload(void)
{
    uint8_t buf[600];
    char name[64];
    char token[64];
    size_t len;

    len = ptty_hello_encode(buf, sizeof(buf), "psxterm/0.1.0", "secret");
    PSX_CHECK_EQ(len, 2 + strlen("psxterm/0.1.0") + strlen("secret"));
    PSX_CHECK_EQ(ptty_hello_decode(buf, len, name, sizeof(name), token,
                                   sizeof(token)),
                 0);
    PSX_CHECK_STR_EQ(name, "psxterm/0.1.0");
    PSX_CHECK_STR_EQ(token, "secret");

    /* empty token */
    len = ptty_hello_encode(buf, sizeof(buf), "client", "");
    PSX_CHECK_EQ(ptty_hello_decode(buf, len, name, sizeof(name), token,
                                   sizeof(token)),
                 0);
    PSX_CHECK_STR_EQ(name, "client");
    PSX_CHECK_STR_EQ(token, "");

    /* truncated payloads are rejected */
    PSX_CHECK_EQ(ptty_hello_decode(buf, 0, name, sizeof(name), token, sizeof(token)), -1);
    PSX_CHECK_EQ(ptty_hello_decode(buf, 1, name, sizeof(name), token, sizeof(token)), -1);
    PSX_CHECK_EQ(ptty_hello_decode(buf, len - 1, name, sizeof(name), token,
                                   sizeof(token)),
                 -1);
    /* length mismatch is rejected */
    buf[len] = 'x';
    PSX_CHECK_EQ(ptty_hello_decode(buf, len + 1, name, sizeof(name), token,
                                   sizeof(token)),
                 -1);
    /* too-small output buffers are rejected */
    len = ptty_hello_encode(buf, sizeof(buf), "client", "token");
    PSX_CHECK_EQ(ptty_hello_decode(buf, len, name, 3, token, sizeof(token)), -1);
}

static void
test_resize_payload(void)
{
    uint8_t buf[4];
    uint16_t rows = 0;
    uint16_t cols = 0;

    PSX_CHECK_EQ(ptty_resize_encode(buf, 53, 211), 4);
    PSX_CHECK_EQ(ptty_resize_decode(buf, &rows, &cols), 0);
    PSX_CHECK_EQ(rows, 53);
    PSX_CHECK_EQ(cols, 211);

    memset(buf, 0, sizeof(buf));
    PSX_CHECK_EQ(ptty_resize_decode(buf, &rows, &cols), -1);
}

static void
test_queue_frame(void)
{
    psx_buf_t buf;
    ptty_header_t header = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_STDOUT,
        .flags = 0,
        .session_id = 7,
        .payload_length = 3,
    };
    const uint8_t payload[3] = {'a', 'b', 'c'};

    psx_buf_init(&buf);
    PSX_CHECK_EQ(ptty_queue_frame(&buf, &header, payload), 0);
    PSX_CHECK_EQ(psx_buf_pending(&buf), PTTY_HEADER_SIZE + 3);
    PSX_CHECK_EQ(buf.data[16], 'a');
    PSX_CHECK_EQ(buf.data[18], 'c');
    /* session id in the queued bytes */
    PSX_CHECK_EQ(buf.data[8], 7);
    psx_buf_free(&buf);
}

/* --- reader tests ------------------------------------------------------- */

typedef struct {
    int rx;
    int tx;
} pipe_pair_t;

static void
pair_open(pipe_pair_t *p)
{
    int fds[2];

    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    p->rx = fds[0];
    p->tx = fds[1];

    PSX_CHECK_EQ(psx_set_nonblocking(p->rx, true), 0);
}

static void
pair_close(pipe_pair_t *p)
{
    close(p->rx);
    close(p->tx);
}

static void
test_reader_partial_header(void)
{
    pipe_pair_t p;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t raw[PTTY_HEADER_SIZE];
    ptty_header_t wire = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_PING,
        .flags = 0,
        .session_id = 3,
        .payload_length = 0,
    };

    pair_open(&p);
    ptty_reader_init(&reader, p.rx);
    PSX_CHECK_EQ(ptty_header_encode(&wire, raw), 0);

    /* no data yet */
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_AGAIN);

    /* five bytes: still AGAIN, state preserved */
    PSX_CHECK_EQ(write(p.tx, raw, 5), 5);
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_AGAIN);

    /* rest of the header */
    PSX_CHECK_EQ(write(p.tx, raw + 5, sizeof(raw) - 5),
                 (ssize_t)(sizeof(raw) - 5));
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_OK);
    PSX_CHECK_EQ(header.type, PTTY_MSG_PING);
    PSX_CHECK_EQ(header.session_id, 3);
    PSX_CHECK(payload == NULL);

    ptty_reader_destroy(&reader);
    pair_close(&p);
}

static void
test_reader_partial_payload(void)
{
    pipe_pair_t p;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t frame[PTTY_HEADER_SIZE + 5];
    ptty_header_t wire = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_STDIN,
        .flags = 0,
        .session_id = 0,
        .payload_length = 5,
    };

    pair_open(&p);
    ptty_reader_init(&reader, p.rx);

    PSX_CHECK_EQ(ptty_header_encode(&wire, frame), 0);
    memcpy(frame + PTTY_HEADER_SIZE, "hello", 5);

    /* header + 2 payload bytes */
    PSX_CHECK_EQ(write(p.tx, frame, PTTY_HEADER_SIZE + 2), PTTY_HEADER_SIZE + 2);
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_AGAIN);

    PSX_CHECK_EQ(write(p.tx, frame + PTTY_HEADER_SIZE + 2, 3), 3);
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_OK);
    PSX_CHECK(payload != NULL);
    PSX_CHECK_EQ(memcmp(payload, "hello", 5), 0);

    /* reader is reusable for the next frame */
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_AGAIN);

    ptty_reader_destroy(&reader);
    pair_close(&p);
}

static void
test_reader_back_to_back_frames(void)
{
    pipe_pair_t p;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t buf[64];
    size_t len = 0;
    ptty_header_t a = {.magic = PTTY_MAGIC,
                       .version = PTTY_VERSION,
                       .type = PTTY_MSG_STDOUT,
                       .flags = 0,
                       .session_id = 1,
                       .payload_length = 2};
    ptty_header_t b = {.magic = PTTY_MAGIC,
                       .version = PTTY_VERSION,
                       .type = PTTY_MSG_STDERR,
                       .flags = 0,
                       .session_id = 1,
                       .payload_length = 3};

    pair_open(&p);
    ptty_reader_init(&reader, p.rx);

    PSX_CHECK_EQ(ptty_header_encode(&a, buf), 0);
    memcpy(buf + PTTY_HEADER_SIZE, "ab", 2);
    len = PTTY_HEADER_SIZE + 2;
    PSX_CHECK_EQ(ptty_header_encode(&b, buf + len), 0);
    memcpy(buf + len + PTTY_HEADER_SIZE, "cde", 3);
    len += PTTY_HEADER_SIZE + 3;

    PSX_CHECK_EQ(write(p.tx, buf, len), (ssize_t)len);

    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_OK);
    PSX_CHECK_EQ(header.type, PTTY_MSG_STDOUT);
    PSX_CHECK_EQ(memcmp(payload, "ab", 2), 0);

    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_OK);
    PSX_CHECK_EQ(header.type, PTTY_MSG_STDERR);
    PSX_CHECK_EQ(memcmp(payload, "cde", 3), 0);

    ptty_reader_destroy(&reader);
    pair_close(&p);
}

static void
test_reader_eof_and_protocol_errors(void)
{
    pipe_pair_t p;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t raw[PTTY_HEADER_SIZE];
    ptty_header_t wire = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_PING,
        .flags = 0,
        .session_id = 0,
        .payload_length = 0,
    };

    /* EOF in the middle of a header */
    pair_open(&p);
    ptty_reader_init(&reader, p.rx);
    PSX_CHECK_EQ(ptty_header_encode(&wire, raw), 0);
    PSX_CHECK_EQ(write(p.tx, raw, 4), 4);
    PSX_CHECK_EQ(close(p.tx), 0);
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload), PTTY_READ_EOF);
    ptty_reader_destroy(&reader);
    close(p.rx);

    /* bad magic is a protocol error, not a silent desync */
    pair_open(&p);
    ptty_reader_init(&reader, p.rx);
    memset(raw, 0, sizeof(raw));
    PSX_CHECK_EQ(write(p.tx, raw, sizeof(raw)), (ssize_t)sizeof(raw));
    PSX_CHECK_EQ(ptty_read_frame(&reader, &header, &payload),
                 PTTY_READ_PROTOCOL);
    ptty_reader_destroy(&reader);
    pair_close(&p);
}

static void
test_reader_max_payload(void)
{
    pipe_pair_t p;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t raw[PTTY_HEADER_SIZE];
    uint8_t *big;
    psx_buf_t queued;
    ptty_read_result_t result = PTTY_READ_AGAIN;
    int send_buffer = 1024;
    int partial_reads = 0;
    ptty_header_t wire = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_STDOUT,
        .flags = 0,
        .session_id = 9,
        .payload_length = PTTY_MAX_PAYLOAD,
    };

    pair_open(&p);
    ptty_reader_init(&reader, p.rx);
    psx_buf_init(&queued);
    PSX_CHECK_EQ(psx_set_nonblocking(p.tx, true), 0);
    PSX_CHECK_EQ(setsockopt(p.tx, SOL_SOCKET, SO_SNDBUF, &send_buffer,
                           sizeof(send_buffer)), 0);

    big = malloc(PTTY_MAX_PAYLOAD);
    PSX_CHECK(big != NULL);
    if(big) {
        memset(big, 0x5a, PTTY_MAX_PAYLOAD);
        PSX_CHECK_EQ(ptty_header_encode(&wire, raw), 0);
        PSX_CHECK_EQ(psx_buf_append(&queued, raw, sizeof(raw)), 0);
        PSX_CHECK_EQ(psx_buf_append(&queued, big, PTTY_MAX_PAYLOAD), 0);
        /* Drain while sending: Darwin's socket buffer cannot hold a whole
         * 64 KiB frame, so a blocking write before the first read deadlocks.
         * A small send buffer exercises partial frames on Linux too. */
        for(int pass = 0; pass < 4096 && result == PTTY_READ_AGAIN; pass++) {
            PSX_CHECK(psx_buf_flush(&queued, p.tx) >= 0);
            result = ptty_read_frame(&reader, &header, &payload);
            if(result == PTTY_READ_AGAIN) {
                partial_reads++;
            }
        }
        PSX_CHECK_EQ(result, PTTY_READ_OK);
        PSX_CHECK(partial_reads > 0);
        PSX_CHECK_EQ(psx_buf_pending(&queued), 0);
        if(result == PTTY_READ_OK) {
            PSX_CHECK_EQ(header.payload_length, PTTY_MAX_PAYLOAD);
            PSX_CHECK(payload != NULL);
            if(payload) {
                PSX_CHECK_EQ(memcmp(payload, big, PTTY_MAX_PAYLOAD), 0);
            }
        }
        free(big);
    }

    ptty_reader_destroy(&reader);
    psx_buf_free(&queued);
    pair_close(&p);
}

static void
test_blocking_frame_send(void)
{
    pipe_pair_t p;
    uint8_t buf[PTTY_HEADER_SIZE + 4];
    ssize_t n;
    ptty_header_t header = {
        .magic = PTTY_MAGIC,
        .version = PTTY_VERSION,
        .type = PTTY_MSG_HELLO_ACK,
        .flags = 0,
        .session_id = 0,
        .payload_length = 4,
    };

    pair_open(&p);
    PSX_CHECK_EQ(ptty_send_frame(p.tx, &header, "abcd"), 0);

    n = read(p.rx, buf, sizeof(buf));
    PSX_CHECK_EQ(n, (ssize_t)sizeof(buf));
    PSX_CHECK_EQ(buf[5], PTTY_MSG_HELLO_ACK);
    PSX_CHECK_EQ(memcmp(buf + PTTY_HEADER_SIZE, "abcd", 4), 0);

    pair_close(&p);
}

static void
test_msg_names(void)
{
    PSX_CHECK_STR_EQ(ptty_msg_name(PTTY_MSG_HELLO), "HELLO");
    PSX_CHECK_STR_EQ(ptty_msg_name(PTTY_MSG_RESIZE), "RESIZE");
    PSX_CHECK_STR_EQ(ptty_msg_name(200), "UNKNOWN");
}

int
main(void)
{
    test_header_roundtrip();
    test_header_rejection();
    test_hello_payload();
    test_resize_payload();
    test_queue_frame();
    test_reader_partial_header();
    test_reader_partial_payload();
    test_reader_back_to_back_frames();
    test_reader_eof_and_protocol_errors();
    test_reader_max_payload();
    test_blocking_frame_send();
    test_msg_names();

    return PSX_TEST_SUMMARY();
}
