/*
 * psxterm - host client for the PSXTerm PTTY/1 protocol.
 *
 * POSIX (Linux/macOS) implementation. Windows support is intentionally out of
 * scope for this milestone; the protocol layer is already platform neutral.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "psxterm/diag.h"
#include "psxterm/protocol.h"
#include "psxterm/util.h"
#include "psxterm/version.h"

#define CLIENT_NAME "psxterm/" PSXTERM_VERSION_STRING
#define EXIT_USAGE 64

static struct termios g_saved_termios;
static bool g_raw_mode;
static volatile sig_atomic_t g_winch;

/* Session persistence state, filled from PTTY_MSG_SESSION_INFO. */
static uint8_t g_resume_token[PTTY_RESUME_TOKEN_SIZE];
static bool g_have_token;
static uint32_t g_session_id;
static bool g_print_token;

static void
client_store_session_info(const ptty_header_t *header, const uint8_t *payload)
{
    if(header->payload_length < 4 + PTTY_RESUME_TOKEN_SIZE) {
        return;
    }

    g_session_id = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                   ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    memcpy(g_resume_token, payload + 4, PTTY_RESUME_TOKEN_SIZE);
    g_have_token = true;

    if(g_print_token) {
        char hex[PTTY_RESUME_TOKEN_SIZE * 2 + 1];

        for(size_t i = 0; i < PTTY_RESUME_TOKEN_SIZE; i++) {
            snprintf(hex + i * 2, 3, "%02x", g_resume_token[i]);
        }
        fprintf(stderr, "psxterm: session %u resume token: %s\n", g_session_id,
                hex);
    }
}

static int
hex_decode_token(const char *hex, uint8_t out[PTTY_RESUME_TOKEN_SIZE])
{
    if(strlen(hex) != PTTY_RESUME_TOKEN_SIZE * 2) {
        return -1;
    }

    for(size_t i = 0; i < PTTY_RESUME_TOKEN_SIZE; i++) {
        unsigned value;

        if(sscanf(hex + i * 2, "%2x", &value) != 1) {
            return -1;
        }
        out[i] = (uint8_t)value;
    }

    return 0;
}

static bool
is_all_digits(const char *text)
{
    if(!text || !*text) {
        return false;
    }

    for(const char *p = text; *p; p++) {
        if(*p < '0' || *p > '9') {
            return false;
        }
    }

    return true;
}

static void
print_detach_notice(void)
{
    fprintf(stderr, "\r\n[detached] session %u\r\n", g_session_id);

    if(g_have_token) {
        char hex[PTTY_RESUME_TOKEN_SIZE * 2 + 1];

        for(size_t i = 0; i < PTTY_RESUME_TOKEN_SIZE; i++) {
            snprintf(hex + i * 2, 3, "%02x", g_resume_token[i]);
        }
        fprintf(stderr, "resume with: psxterm attach <host> %u --resume %s\r\n",
                g_session_id, hex);
    } else {
        fprintf(stderr, "the daemon did not provide a resume token\r\n");
    }
}

static void
restore_terminal(void)
{
    if(g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_raw_mode = false;
    }
}

static void
signal_handler(int signo)
{
    if(signo == SIGWINCH) {
        g_winch = 1;
        return;
    }

    restore_terminal();
    _exit(1);
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
            return -1;
        }

        written += (size_t)n;
    }

    return 0;
}

/* Raw-mode terminals need CRLF; do not double the CR of an existing CRLF. */
static int
emit_output(const uint8_t *data, size_t len, bool raw_terminal)
{
    if(!raw_terminal) {
        return write_all(STDOUT_FILENO, data, len);
    }

    for(size_t i = 0; i < len; i++) {
        if(data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) {
            if(write_all(STDOUT_FILENO, (const uint8_t *)"\r\n", 2) < 0) {
                return -1;
            }
        } else if(write_all(STDOUT_FILENO, data + i, 1) < 0) {
            return -1;
        }
    }

    return 0;
}

static void
terminal_size(uint16_t *rows, uint16_t *cols)
{
    struct winsize ws;

    *rows = 24;
    *cols = 80;

    if(isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if(ws.ws_row > 0) {
            *rows = ws.ws_row;
        }
        if(ws.ws_col > 0) {
            *cols = ws.ws_col;
        }
    }
}

static int
wait_frame(ptty_reader_t *reader, int fd, ptty_header_t *header,
           const uint8_t **payload, int timeout_ms)
{
    uint64_t deadline = psx_now_ms() + (uint64_t)timeout_ms;

    for(;;) {
        ptty_read_result_t rc = ptty_read_frame(reader, header, payload);

        if(rc == PTTY_READ_OK) {
            return 0;
        }
        if(rc != PTTY_READ_AGAIN) {
            return -1;
        }

        {
            int64_t remaining = (int64_t)deadline - (int64_t)psx_now_ms();
            struct pollfd pfd = {.fd = fd, .events = POLLIN};

            if(remaining <= 0) {
                return -2;
            }
            if(poll(&pfd, 1, (int)remaining) < 0 && errno != EINTR) {
                return -1;
            }
        }
    }
}

static int
connect_host(const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char port_str[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if(getaddrinfo(host, port_str, &hints, &result) != 0) {
        fprintf(stderr, "psxterm: cannot resolve %s\n", host);
        return -1;
    }

    for(ai = result; ai; ai = ai->ai_next) {
        if((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
            continue;
        }

        if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if(fd < 0) {
        fprintf(stderr, "psxterm: cannot connect to %s:%u: %s\n", host,
                (unsigned)port, strerror(errno));
    }

    return fd;
}

static int
send_frame(int fd, uint8_t type, const void *payload, uint32_t len)
{
    return ptty_send_simple(fd, type, 0, 0, payload, len);
}

/*
 * Exec mode: run one command line, forward stdin, and wait for the server's
 * EXIT(SHELL) marker. Returns the remote status, or 1 on protocol failure.
 */
static int
run_exec(int fd, ptty_reader_t *reader, const char *command)
{
    bool stdin_open = true;
    ptty_header_t header;
    const uint8_t *payload = NULL;

    if(send_frame(fd, PTTY_MSG_EXEC, command, (uint32_t)strlen(command)) < 0) {
        fprintf(stderr, "psxterm: send failed: %s\n", strerror(errno));
        return 1;
    }

    for(;;) {
        struct pollfd pfds[2];
        int rc;

        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = stdin_open ? POLLIN : 0;
        pfds[1].fd = fd;
        pfds[1].events = POLLIN;

        rc = poll(pfds, 2, 30000);

        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            fprintf(stderr, "psxterm: poll: %s\n", strerror(errno));
            return 1;
        }
        if(rc == 0) {
            fprintf(stderr, "psxterm: timed out waiting for the command\n");
            return 1;
        }

        if(stdin_open && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t buffer[4096];
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));

            if(n > 0) {
                if(send_frame(fd, PTTY_MSG_STDIN, buffer, (uint32_t)n) < 0) {
                    return 1;
                }
            } else {
                send_frame(fd, PTTY_MSG_STDIN, NULL, 0);
                stdin_open = false;
            }
        }

        if(!(pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }

        for(;;) {
            ptty_read_result_t r = ptty_read_frame(reader, &header, &payload);

            if(r == PTTY_READ_AGAIN) {
                break;
            }
            if(r != PTTY_READ_OK) {
                fprintf(stderr, "psxterm: connection lost\n");
                return 1;
            }

            switch(header.type) {
            case PTTY_MSG_STDOUT:
                emit_output(payload, header.payload_length, false);
                break;

            case PTTY_MSG_STDERR:
                write_all(STDERR_FILENO, payload, header.payload_length);
                break;

            case PTTY_MSG_EXIT:
                if(header.payload_length >= 5 &&
                   payload[4] == PTTY_EXIT_SHELL) {
                    int32_t status = (int32_t)((uint32_t)payload[0] |
                                               ((uint32_t)payload[1] << 8) |
                                               ((uint32_t)payload[2] << 16) |
                                               ((uint32_t)payload[3] << 24));
                    send_frame(fd, PTTY_MSG_CLOSE, NULL, 0);
                    return status;
                }
                break;

            case PTTY_MSG_CLOSE:
                return 0;

            case PTTY_MSG_PING:
                send_frame(fd, PTTY_MSG_PONG, payload, header.payload_length);
                break;

            default:
                break;
            }
        }
    }
}

static int
run_interactive(int fd, ptty_reader_t *reader, bool use_raw)
{
    bool stdin_open = true;
    bool raw_terminal = false;
    bool stdin_is_tty = isatty(STDIN_FILENO);
    uint64_t drain_deadline = 0;
    struct pollfd pfds[2];

    if(use_raw && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        struct termios raw = g_saved_termios;

        /* Raw mode, set explicitly so the client builds on every POSIX
         * platform without relying on cfmakeraw() visibility rules. */
        raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                         ICRNL | IXON);
        raw.c_oflag &= ~OPOST;
        raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw.c_cflag &= ~(CSIZE | PARENB);
        raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        if(tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
            g_raw_mode = true;
            raw_terminal = true;
        }
    }

    atexit(restore_terminal);
    signal(SIGWINCH, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    for(;;) {
        int rc;

        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = stdin_open ? POLLIN : 0;
        pfds[1].fd = fd;
        pfds[1].events = POLLIN;

        rc = poll(pfds, 2, 100);

        if(rc < 0) {
            if(errno == EINTR) {
                /* fall through to the SIGWINCH check */
            } else {
                fprintf(stderr, "psxterm: poll: %s\n", strerror(errno));
                break;
            }
        }

        if(g_winch) {
            uint8_t payload[4];
            uint16_t rows;
            uint16_t cols;

            g_winch = 0;
            terminal_size(&rows, &cols);
            ptty_resize_encode(payload, rows, cols);
            send_frame(fd, PTTY_MSG_RESIZE, payload, sizeof(payload));
        }

        if(rc > 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ptty_header_t header;
            const uint8_t *payload = NULL;

            for(;;) {
                ptty_read_result_t r = ptty_read_frame(reader, &header, &payload);

                if(r == PTTY_READ_AGAIN) {
                    break;
                }
                if(r != PTTY_READ_OK) {
                    return 0;
                }

                if(drain_deadline) {
                    drain_deadline = psx_now_ms() + 2000;
                }

                switch(header.type) {
                case PTTY_MSG_STDOUT:
                    emit_output(payload, header.payload_length, raw_terminal);
                    break;

                case PTTY_MSG_STDERR:
                    emit_output(payload, header.payload_length, raw_terminal);
                    break;

                case PTTY_MSG_EXIT: {
                    int32_t status = 0;

                    if(header.payload_length >= 4) {
                        status = (int32_t)((uint32_t)payload[0] |
                                           ((uint32_t)payload[1] << 8) |
                                           ((uint32_t)payload[2] << 16) |
                                           ((uint32_t)payload[3] << 24));
                    }
                    if(header.payload_length >= 5 &&
                       payload[4] == PTTY_EXIT_PROCESS && status != 0) {
                        char notice[64];
                        int len = snprintf(notice, sizeof(notice),
                                           "\r\n[exit %d]\r\n", (int)status);
                        emit_output((const uint8_t *)notice, (size_t)len,
                                    raw_terminal);
                    }
                    break;
                }

                case PTTY_MSG_PING:
                    send_frame(fd, PTTY_MSG_PONG, payload,
                               header.payload_length);
                    break;

                case PTTY_MSG_SESSION_INFO:
                    client_store_session_info(&header, payload);
                    break;

                case PTTY_MSG_CAPS:
                    break;

                case PTTY_MSG_CLOSE:
                    return 0;

                default:
                    break;
                }
            }
        }

        if(rc > 0 && stdin_open && (pfds[0].revents & (POLLIN | POLLHUP))) {
            uint8_t buffer[4096];
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));

            if(n > 0) {
                ssize_t detach_at = -1;

                for(ssize_t i = 0; i < n; i++) {
                    if(buffer[i] == 0x1d) { /* Ctrl+] detaches */
                        detach_at = i;
                        break;
                    }
                }

                if(detach_at >= 0) {
                    if(detach_at > 0) {
                        send_frame(fd, PTTY_MSG_STDIN, buffer,
                                   (uint32_t)detach_at);
                    }
                    send_frame(fd, PTTY_MSG_DETACH, NULL, 0);
                    restore_terminal();
                    print_detach_notice();
                    return 0;
                }

                if(send_frame(fd, PTTY_MSG_STDIN, buffer, (uint32_t)n) < 0) {
                    break;
                }
            } else if(n == 0) {
                /* stdin EOF: tell the remote side, then keep draining until
                 * the server closes the session (bounded by a quiet-period
                 * deadline in batch mode). */
                send_frame(fd, PTTY_MSG_STDIN, NULL, 0);
                stdin_open = false;

                if(!stdin_is_tty) {
                    drain_deadline = psx_now_ms() + 2000;
                }
            } else if(errno != EINTR && errno != EAGAIN) {
                stdin_open = false;
            }
        }

        if(drain_deadline && psx_now_ms() >= drain_deadline) {
            break;
        }
    }

    restore_terminal();
    send_frame(fd, PTTY_MSG_CLOSE, NULL, 0);

    return 0;
}

/* PING/PONG roundtrip used by the doctor to validate the live connection. */
static int
doctor_ping(int fd, ptty_reader_t *reader, int timeout_ms)
{
    static const char token[] = "psxterm-doctor";
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint64_t deadline = psx_now_ms() + (uint64_t)timeout_ms;

    if(send_frame(fd, PTTY_MSG_PING, token, sizeof(token) - 1) < 0) {
        return -1;
    }

    while(psx_now_ms() < deadline) {
        if(wait_frame(reader, fd, &header, &payload,
                      (int)(deadline - psx_now_ms())) != 0) {
            return -1;
        }

        if(header.type == PTTY_MSG_PONG) {
            return header.payload_length == sizeof(token) - 1 &&
                           memcmp(payload, token, sizeof(token) - 1) == 0
                       ? 0
                       : -1;
        }
        if(header.type == PTTY_MSG_CLOSE) {
            return -1;
        }
    }

    return -1;
}

/*
 * Doctor mode: stream the remote diagnostic report to stdout and exit with
 * the server's overall status (0 ready, 1 warnings, 2 not ready).
 */
static int
run_doctor(int fd, ptty_reader_t *reader, bool json)
{
    uint8_t flags = json ? PTTY_DIAG_FLAG_JSON : 0;
    ptty_header_t header;
    const uint8_t *payload = NULL;

    if(doctor_ping(fd, reader, 2000) < 0) {
        fprintf(stderr, "psxterm: PING/PONG roundtrip failed\n");
        return PSX_DIAG_EXIT_FAILED;
    }

    if(send_frame(fd, PTTY_MSG_DIAG_REQUEST, &flags, 1) < 0) {
        fprintf(stderr, "psxterm: cannot request diagnostics: %s\n",
                strerror(errno));
        return PSX_DIAG_EXIT_FAILED;
    }

    for(;;) {
        if(wait_frame(reader, fd, &header, &payload, 60000) != 0) {
            fprintf(stderr, "psxterm: diagnostics timed out\n");
            return PSX_DIAG_EXIT_FAILED;
        }

        switch(header.type) {
        case PTTY_MSG_DIAG_DATA:
            write_all(STDOUT_FILENO, payload, header.payload_length);
            break;

        case PTTY_MSG_DIAG_DONE: {
            int status = header.payload_length >= 1 ? payload[0]
                                                    : PSX_DIAG_EXIT_FAILED;

            send_frame(fd, PTTY_MSG_CLOSE, NULL, 0);
            return status;
        }

        case PTTY_MSG_PING:
            send_frame(fd, PTTY_MSG_PONG, payload, header.payload_length);
            break;

        case PTTY_MSG_CLOSE:
            return PSX_DIAG_EXIT_FAILED;

        default:
            break;
        }
    }
}

/* Lists the daemon's sessions and exits. */
static int
run_sessions(int fd, ptty_reader_t *reader)
{
    ptty_header_t header;
    const uint8_t *payload = NULL;

    if(send_frame(fd, PTTY_MSG_SESSIONS_REQUEST, NULL, 0) < 0) {
        fprintf(stderr, "psxterm: cannot request the session list: %s\n",
                strerror(errno));
        return PSX_DIAG_EXIT_FAILED;
    }

    for(;;) {
        if(wait_frame(reader, fd, &header, &payload, 15000) != 0) {
            fprintf(stderr, "psxterm: session list timed out\n");
            return PSX_DIAG_EXIT_FAILED;
        }

        switch(header.type) {
        case PTTY_MSG_SESSIONS_DATA:
            write_all(STDOUT_FILENO, payload, header.payload_length);
            break;

        case PTTY_MSG_SESSIONS_DONE:
            send_frame(fd, PTTY_MSG_CLOSE, NULL, 0);
            return 0;

        case PTTY_MSG_PING:
            send_frame(fd, PTTY_MSG_PONG, payload, header.payload_length);
            break;

        case PTTY_MSG_CLOSE:
            return 0;

        default:
            /* The shell prompt and other frames are not part of the list. */
            break;
        }
    }
}

/* Resumes a detached session and enters the interactive loop. */
static int
run_attach(int fd, ptty_reader_t *reader, uint32_t session_id,
           const char *resume_hex, bool use_raw)
{
    uint8_t payload[PTTY_ATTACH_PAYLOAD_SIZE];

    if(!resume_hex || hex_decode_token(resume_hex, payload + 4) < 0) {
        fprintf(stderr,
                "psxterm: attach requires --resume TOKEN (32 hex characters)\n");
        return EXIT_USAGE;
    }

    payload[0] = (uint8_t)(session_id & 0xff);
    payload[1] = (uint8_t)((session_id >> 8) & 0xff);
    payload[2] = (uint8_t)((session_id >> 16) & 0xff);
    payload[3] = (uint8_t)((session_id >> 24) & 0xff);

    if(send_frame(fd, PTTY_MSG_ATTACH, payload, sizeof(payload)) < 0) {
        fprintf(stderr, "psxterm: cannot request attach: %s\n",
                strerror(errno));
        return PSX_DIAG_EXIT_FAILED;
    }

    for(;;) {
        ptty_header_t header;
        const uint8_t *data = NULL;

        if(wait_frame(reader, fd, &header, &data, 10000) != 0) {
            fprintf(stderr, "psxterm: attach timed out\n");
            return PSX_DIAG_EXIT_FAILED;
        }

        switch(header.type) {
        case PTTY_MSG_ATTACH_OK:
            g_session_id = session_id;
            if(header.payload_length >= 5 && data[4]) {
                fprintf(stderr,
                        "psxterm: output was truncated while detached\n");
            }
            return run_interactive(fd, reader, use_raw);

        case PTTY_MSG_ATTACH_FAIL: {
            const char *reason = "unknown session";

            if(header.payload_length >= 1) {
                switch(data[0]) {
                case PTTY_ATTACH_BAD_TOKEN:
                    reason = "resume token rejected";
                    break;
                case PTTY_ATTACH_NOT_DETACHED:
                    reason = "session is not detached";
                    break;
                case PTTY_ATTACH_LIMIT:
                    reason = "daemon session limit reached";
                    break;
                case PTTY_ATTACH_UNKNOWN_SESSION:
                default:
                    reason = "unknown session";
                    break;
                }
            }

            fprintf(stderr, "psxterm: cannot attach to session %u: %s\n",
                    session_id, reason);
            return PSX_DIAG_EXIT_FAILED;
        }

        case PTTY_MSG_SESSION_INFO:
            client_store_session_info(&header, data);
            break;

        case PTTY_MSG_PING:
            send_frame(fd, PTTY_MSG_PONG, data, header.payload_length);
            break;

        case PTTY_MSG_CLOSE:
            return PSX_DIAG_EXIT_FAILED;

        default:
            break;
        }
    }
}

static void
usage(const char *argv0)
{
    printf("usage: %s [options] <host>\n", argv0);
    printf("       %s doctor [--json] [options] <host>\n", argv0);
    printf("       %s sessions [options] <host>\n", argv0);
    printf("       %s attach [options] <host> <session> --resume TOKEN\n",
           argv0);
    printf("\n");
    printf("Connect to a PSXTerm daemon on a PS4/PS5.\n");
    printf("\n");
    printf("  -p, --port PORT     TCP port (default %d)\n", PSXTERM_DEFAULT_PORT);
    printf("  -t, --token TOKEN   daemon authentication token\n");
    printf("  -e, --exec CMD      run one command and exit\n");
    printf("      --no-raw        do not switch the local terminal to raw mode\n");
    printf("      doctor          run remote diagnostics (exit 0 ready, 1\n");
    printf("                      warnings, 2 not ready)\n");
    printf("      sessions        list the daemon's sessions\n");
    printf("      attach          resume a detached session\n");
    printf("      --resume TOKEN  32 hex character resume token for attach\n");
    printf("      --print-token   print the session resume token at startup\n");
    printf("      --json          machine-readable diagnostics output\n");
    printf("  -V, --version       print version\n");
    printf("  -h, --help          this help\n");
    printf("\n");
    printf("In a session, Ctrl+] detaches and prints how to resume it.\n");
}

int
main(int argc, char **argv)
{
    uint16_t port = PSXTERM_DEFAULT_PORT;
    const char *token = NULL;
    const char *exec_command = NULL;
    const char *host = NULL;
    const char *resume = NULL;
    bool use_raw = true;
    bool doctor_mode = false;
    bool sessions_mode = false;
    bool attach_mode = false;
    uint32_t attach_id = 0;
    bool json = false;
    int fd;
    ptty_reader_t reader;
    ptty_header_t header;
    const uint8_t *payload = NULL;
    uint8_t hello[600];
    size_t hello_len;

    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if(strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            if(++i >= argc || !psx_parse_u16(argv[i], &port) || port == 0) {
                fprintf(stderr, "psxterm: invalid port\n");
                return EXIT_USAGE;
            }
        } else if(strcmp(arg, "-t") == 0 || strcmp(arg, "--token") == 0) {
            if(++i >= argc) {
                return EXIT_USAGE;
            }
            token = argv[i];
        } else if(strcmp(arg, "-e") == 0 || strcmp(arg, "--exec") == 0) {
            if(++i >= argc) {
                return EXIT_USAGE;
            }
            exec_command = argv[i];
        } else if(strcmp(arg, "--no-raw") == 0) {
            use_raw = false;
        } else if(strcmp(arg, "--json") == 0) {
            json = true;
        } else if(strcmp(arg, "doctor") == 0 && !host) {
            doctor_mode = true;
        } else if(strcmp(arg, "sessions") == 0 && !host) {
            sessions_mode = true;
        } else if(strcmp(arg, "attach") == 0 && !host) {
            attach_mode = true;
        } else if(strcmp(arg, "--resume") == 0) {
            if(++i >= argc) {
                return EXIT_USAGE;
            }
            resume = argv[i];
        } else if(strcmp(arg, "--print-token") == 0) {
            g_print_token = true;
        } else if(attach_mode && attach_id == 0 && is_all_digits(arg)) {
            attach_id = (uint32_t)strtoul(arg, NULL, 10);
        } else if(strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("psxterm %s (protocol %s)\n", PSXTERM_VERSION_STRING,
                   PSXTERM_PROTOCOL_NAME);
            return 0;
        } else if(strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if(arg[0] == '-') {
            fprintf(stderr, "psxterm: unknown option: %s\n", arg);
            usage(argv[0]);
            return EXIT_USAGE;
        } else if(!host) {
            host = arg;
        } else {
            fprintf(stderr, "psxterm: unexpected argument: %s\n", arg);
            return EXIT_USAGE;
        }
    }

    if(!host) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    /* Writes to a closed server socket must fail with EPIPE, not kill us. */
    signal(SIGPIPE, SIG_IGN);

    if((fd = connect_host(host, port)) < 0) {
        return 1;
    }

    ptty_reader_init(&reader, fd);

    /* HELLO */
    if(!(hello_len = ptty_hello_encode(hello, sizeof(hello), CLIENT_NAME,
                                       token))) {
        fprintf(stderr, "psxterm: cannot encode HELLO\n");
        close(fd);
        return 1;
    }

    if(send_frame(fd, PTTY_MSG_HELLO, hello, (uint32_t)hello_len) < 0 ||
       wait_frame(&reader, fd, &header, &payload, 5000) != 0 ||
       header.type != PTTY_MSG_HELLO_ACK) {
        fprintf(stderr, "psxterm: handshake failed\n");
        close(fd);
        return 1;
    }

    if(header.payload_length < 1 || payload[0] != PTTY_ACK_OK) {
        int status = header.payload_length >= 1 ? payload[0] : -1;

        if(status == PTTY_ACK_AUTH_REQUIRED ||
           status == PTTY_ACK_AUTH_FAILED) {
            fprintf(stderr, "psxterm: authentication failed\n");
        } else if(status == PTTY_ACK_SERVER_BUSY) {
            fprintf(stderr, "psxterm: server busy (session limit reached)\n");
        } else {
            fprintf(stderr, "psxterm: server refused the connection\n");
        }

        close(fd);
        return 1;
    }

    /* "PS5|PSXTerm 0.1.0" */
    {
        char text[160];
        size_t len = header.payload_length - 1;
        char *sep;

        if(len >= sizeof(text)) {
            len = sizeof(text) - 1;
        }
        memcpy(text, payload + 1, len);
        text[len] = '\0';

        sep = strchr(text, '|');
        if(sep && !json) {
            *sep = '\0';
            if(strcmp(text, "PS5") == 0) {
                printf("Connected to PlayStation 5\n");
            } else if(strcmp(text, "PS4") == 0) {
                printf("Connected to PlayStation 4\n");
            } else {
                printf("Connected to %s\n", text);
            }
            printf("%s\n", sep + 1);
        }
    }

    /* OPEN with window size and TERM (not used when attaching). */
    if(!attach_mode)
    {
        uint8_t open_payload[4 + 64];
        uint16_t rows;
        uint16_t cols;
        const char *term = getenv("TERM");
        size_t term_len = term ? strlen(term) : 0;

        if(term_len > 63) {
            term_len = 63;
        }

        terminal_size(&rows, &cols);
        ptty_resize_encode(open_payload, rows, cols);
        if(term_len) {
            memcpy(open_payload + 4, term, term_len);
        }

        if(send_frame(fd, PTTY_MSG_OPEN, open_payload,
                      (uint32_t)(4 + term_len)) < 0 ||
           wait_frame(&reader, fd, &header, &payload, 5000) != 0 ||
           header.type != PTTY_MSG_OPEN_OK) {
            fprintf(stderr, "psxterm: cannot open session\n");
            close(fd);
            return 1;
        }
    }

    psx_set_nonblocking(fd, true);

    if(attach_mode) {
        int status;

        if(attach_id == 0 || !resume) {
            fprintf(stderr,
                    "psxterm: attach needs a session id and --resume TOKEN\n");
            close(fd);
            return EXIT_USAGE;
        }

        fflush(stdout);
        status = run_attach(fd, &reader, attach_id, resume, use_raw);
        close(fd);

        return status;
    }

    if(sessions_mode) {
        int status;

        fflush(stdout);
        status = run_sessions(fd, &reader);
        close(fd);

        return status;
    }

    if(doctor_mode) {
        int status;

        fflush(stdout);
        status = run_doctor(fd, &reader, json);
        close(fd);

        return status;
    }

    if(exec_command) {
        int status = run_exec(fd, &reader, exec_command);

        restore_terminal();
        close(fd);

        return status;
    }

    run_interactive(fd, &reader, use_raw);
    close(fd);

    return 0;
}
