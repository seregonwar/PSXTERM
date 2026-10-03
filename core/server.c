#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/diag.h"
#include "psxterm/instance.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/protocol.h"
#include "psxterm/runtime.h"
#include "psxterm/server.h"
#include "psxterm/session.h"
#include "psxterm/shell.h"
#include "psxterm/version.h"

#define SERVER_BACKLOG 16
#define SERVER_TICK_MS 50
#define SERVER_CLOSE_GRACE_MS 2000
#define SERVER_DEFAULT_HANDSHAKE_MS 10000

static volatile sig_atomic_t g_shutdown;
static int g_signal_pipe[2] = {-1, -1};

static void
signal_handler(int signo)
{
    uint8_t byte = (uint8_t)signo;

    if(g_signal_pipe[1] >= 0) {
        ssize_t ignored = write(g_signal_pipe[1], &byte, 1);
        (void)ignored;
    }
}

typedef struct {
    const psx_server_config_t *config;
    psx_session_manager_t sessions;
    int listen_fd;
    int signal_pipe[2];

    struct pollfd *pfds;
    uint32_t *owners; /* session id per pollfd; 0 for listen/signal */
    size_t pfds_cap;
} psx_server_t;

void
psx_server_config_default(psx_server_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->port = PSXTERM_DEFAULT_PORT;
    config->bind_addr = "0.0.0.0";
    config->max_sessions = PSXTERM_MAX_SESSIONS_DEFAULT;
    config->auth_mode = PSX_AUTH_NONE;
    config->handshake_timeout_ms = SERVER_DEFAULT_HANDSHAKE_MS;
    config->idle_timeout_ms = 0;
    config->persist_sessions = true;
    config->detached_timeout_ms = 30 * 60 * 1000;
}

/* --- handshake ---------------------------------------------------------- */

static int
send_hello_ack(int fd, uint8_t status)
{
    char text[128];
    int len = snprintf(text, sizeof(text), "%s|PSXTerm %s",
                       psx_platform_name(), PSXTERM_VERSION_STRING);

    if(len < 0 || (size_t)len + 1 > sizeof(text)) {
        return -1;
    }

    {
        uint8_t payload[160];

        payload[0] = status;
        memcpy(payload + 1, text, (size_t)len);

        return ptty_send_simple(fd, PTTY_MSG_HELLO_ACK, 0, 0, payload,
                                (uint32_t)len + 1);
    }
}

static int
handshake_hello(psx_server_t *server, psx_session_t *session,
                const ptty_header_t *header, const uint8_t *payload)
{
    char name[64];
    char token[128];

    if(ptty_hello_decode(payload, header->payload_length, name, sizeof(name),
                         token, sizeof(token)) < 0) {
        PSX_LOGW("session %u: malformed HELLO", session->id);
        return -1;
    }

    snprintf(session->client_name, sizeof(session->client_name), "%s",
             name[0] ? name : "unknown");

    if(server->config->auth_mode == PSX_AUTH_TOKEN) {
        size_t expected = strlen(server->config->auth_token);
        size_t given = strlen(token);
        unsigned diff = expected != given;

        for(size_t i = 0; i < expected && i < given; i++) {
            diff |= (unsigned)(server->config->auth_token[i] ^ token[i]);
        }

        if(diff != 0) {
            PSX_LOGW("session %u: authentication failed for client '%s'",
                     session->id, session->client_name);
            send_hello_ack(session->sock_fd, PTTY_ACK_AUTH_FAILED);
            return -1;
        }
    }

    session->state = PSX_SESSION_HANDSHAKING;
    PSX_NOTIFY("session %u: client \"%s\" connected", session->id,
               session->client_name);

    return send_hello_ack(session->sock_fd, PTTY_ACK_OK);
}

static int
handshake_open(psx_server_t *server, psx_session_t *session,
               const ptty_header_t *header, const uint8_t *payload)
{
    uint8_t open_ok[4];
    uint32_t id = session->id;

    (void)server;

    if(session->state != PSX_SESSION_HANDSHAKING) {
        PSX_LOGW("session %u: OPEN before HELLO", session->id);
        return -1;
    }

    if(header->payload_length >= 4) {
        uint16_t rows = 0;
        uint16_t cols = 0;

        if(ptty_resize_decode(payload, &rows, &cols) == 0) {
            session->rows = rows;
            session->cols = cols;
        }
    }

    if(header->payload_length > 4) {
        size_t term_len = header->payload_length - 4;

        if(term_len >= sizeof(session->term)) {
            term_len = sizeof(session->term) - 1;
        }
        memcpy(session->term, payload + 4, term_len);
        session->term[term_len] = '\0';
    }

    open_ok[0] = (uint8_t)(id & 0xff);
    open_ok[1] = (uint8_t)((id >> 8) & 0xff);
    open_ok[2] = (uint8_t)((id >> 16) & 0xff);
    open_ok[3] = (uint8_t)((id >> 24) & 0xff);

    if(psx_session_emit(session, PTTY_MSG_OPEN_OK, open_ok, sizeof(open_ok)) < 0) {
        return -1;
    }

    /*
     * Hand the creating client the resume token over the authenticated
     * connection. The token is a secret: it is never written to the daemon
     * log, and the client only prints it when the user detaches.
     */
    {
        uint8_t info[4 + PTTY_RESUME_TOKEN_SIZE];

        info[0] = (uint8_t)(id & 0xff);
        info[1] = (uint8_t)((id >> 8) & 0xff);
        info[2] = (uint8_t)((id >> 16) & 0xff);
        info[3] = (uint8_t)((id >> 24) & 0xff);
        memcpy(info + 4, session->resume_token, PTTY_RESUME_TOKEN_SIZE);

        if(psx_session_emit(session, PTTY_MSG_SESSION_INFO, info,
                            sizeof(info)) < 0) {
            return -1;
        }
    }

    if(psx_session_begin(session) < 0) {
        PSX_LOGE("session %u: failed to start: %s", session->id,
                 strerror(errno));
        return -1;
    }

    return 0;
}

static int
handshake_attach(psx_server_t *server, psx_session_t *session,
                 const ptty_header_t *header, const uint8_t *payload);

static int
server_handshake_frame(psx_server_t *server, psx_session_t *session,
                       const ptty_header_t *header, const uint8_t *payload)
{
    switch(header->type) {
    case PTTY_MSG_HELLO:
        return handshake_hello(server, session, header, payload);

    case PTTY_MSG_OPEN:
        return handshake_open(server, session, header, payload);

    case PTTY_MSG_ATTACH:
        return handshake_attach(server, session, header, payload);

    case PTTY_MSG_PING:
        return psx_session_emit(session, PTTY_MSG_PONG, payload,
                                header->payload_length) < 0
                   ? PSX_SESSION_FRAME_ERROR
                   : PSX_SESSION_FRAME_CONTINUE;

    case PTTY_MSG_SHUTDOWN:
        PSX_LOGI("session %u: shutdown requested by client", session->id);
        ptty_send_simple(session->sock_fd, PTTY_MSG_CLOSE, 0, session->id, NULL,
                         0);
        g_shutdown = 1;
        return PSX_SESSION_FRAME_CLOSE;

    case PTTY_MSG_CLOSE:
        return PSX_SESSION_FRAME_ERROR;

    default:
        PSX_LOGW("session %u: unexpected %s frame during handshake",
                 session->id, ptty_msg_name(header->type));
        return PSX_SESSION_FRAME_ERROR;
    }
}

static uint32_t
read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void
send_attach_fail(int fd, uint8_t reason, uint32_t session_id)
{
    ptty_send_simple(fd, PTTY_MSG_ATTACH_FAIL, 0, session_id, &reason, 1);
}

/*
 * Resume a detached session on this connection. The handshaking session is a
 * placeholder: once the token checks out, the socket is handed to the target
 * session and the placeholder is removed without touching the socket.
 */
static int
handshake_attach(psx_server_t *server, psx_session_t *session,
                 const ptty_header_t *header, const uint8_t *payload)
{
    psx_session_t *target;
    uint32_t id;
    int fd;

    if(header->payload_length != PTTY_ATTACH_PAYLOAD_SIZE) {
        send_attach_fail(session->sock_fd, PTTY_ATTACH_UNKNOWN_SESSION, 0);
        return PSX_SESSION_FRAME_ERROR;
    }

    id = read_le32(payload);
    target = psx_session_manager_find(&server->sessions, id);

    if(!target || target == session) {
        send_attach_fail(session->sock_fd, PTTY_ATTACH_UNKNOWN_SESSION, id);
        return PSX_SESSION_FRAME_ERROR;
    }

    if(!psx_session_is_detached(target)) {
        send_attach_fail(session->sock_fd, PTTY_ATTACH_NOT_DETACHED, id);
        return PSX_SESSION_FRAME_ERROR;
    }

    if(!psx_session_check_token(target, payload + 4, PTTY_RESUME_TOKEN_SIZE)) {
        PSX_LOGW("session %u: attach to session %u rejected (bad token)",
                 session->id, id);
        send_attach_fail(session->sock_fd, PTTY_ATTACH_BAD_TOKEN, id);
        return PSX_SESSION_FRAME_ERROR;
    }

    {
        uint8_t ok[5];

        ok[0] = (uint8_t)(id & 0xff);
        ok[1] = (uint8_t)((id >> 8) & 0xff);
        ok[2] = (uint8_t)((id >> 16) & 0xff);
        ok[3] = (uint8_t)((id >> 24) & 0xff);
        ok[4] = target->scrollback_truncated ? 1 : 0;

        if(ptty_send_simple(session->sock_fd, PTTY_MSG_ATTACH_OK, 0, id, ok,
                            sizeof(ok)) < 0) {
            return PSX_SESSION_FRAME_ERROR;
        }
    }

    fd = session->sock_fd;
    session->sock_fd = -1;
    session->state = PSX_SESSION_CLOSED;

    if(psx_session_attach(target, fd) < 0) {
        PSX_LOGE("session %u: cannot attach: %s", id, strerror(errno));
        close(fd);
        return PSX_SESSION_FRAME_ERROR;
    }

    PSX_NOTIFY("session %u: client \"%s\" attached", target->id,
               target->client_name[0] ? target->client_name : "unknown");

    return PSX_SESSION_FRAME_REPLACED;
}

/* --- session event handling --------------------------------------------- */

static int
server_session_readable(psx_server_t *server, psx_session_t *session)
{
    for(;;) {
        ptty_header_t header;
        const uint8_t *payload = NULL;
        ptty_read_result_t rc;
        int handled;

        rc = ptty_read_frame(&session->reader, &header, &payload);

        if(rc == PTTY_READ_AGAIN) {
            return 0;
        }
        if(rc == PTTY_READ_EOF) {
            PSX_LOGD("session %u: client disconnected", session->id);
            return PSX_SESSION_FRAME_DETACH;
        }
        if(rc == PTTY_READ_ERROR) {
            PSX_LOGW("session %u: socket read: %s", session->id,
                     strerror(errno));
            return PSX_SESSION_FRAME_DETACH;
        }
        if(rc == PTTY_READ_PROTOCOL) {
            session->protocol_errors++;
            PSX_LOGW("session %u: protocol error", session->id);
            return PSX_SESSION_FRAME_ERROR;
        }

        session->frames_in++;
        psx_session_touch(session);

        if(session->state == PSX_SESSION_RUNNING ||
           session->state == PSX_SESSION_CLOSING) {
            handled = psx_session_handle_frame(session, &header, payload);
        } else {
            handled = server_handshake_frame(server, session, &header, payload);
        }

        if(handled != 0) {
            return handled;
        }
    }
}

static void
server_session_remove(psx_server_t *server, psx_session_t *session,
                      const char *reason)
{
    PSX_NOTIFY("session %u: closed (%s)", session->id, reason);
    psx_session_manager_remove(&server->sessions, session);
}

static void
server_session_start_closing(psx_session_t *session)
{
    if(session->state == PSX_SESSION_CLOSING ||
       session->state == PSX_SESSION_CLOSED) {
        return;
    }

    if(session->shell && psh_shell_exit_requested(session->shell)) {
        psx_session_emit_exit(session, psh_shell_exit_code(session->shell),
                              PTTY_EXIT_SHELL);
    }

    psx_session_emit(session, PTTY_MSG_CLOSE, NULL, 0);
    session->state = PSX_SESSION_CLOSING;
    session->close_deadline_ms = psx_now_ms() + SERVER_CLOSE_GRACE_MS;
}

static void
server_accept(psx_server_t *server)
{
    for(;;) {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        psx_session_t *session;
        int fd;
        int one = 1;

        fd = accept(server->listen_fd, (struct sockaddr *)&addr, &addr_len);

        if(fd < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if(errno == EINTR) {
                continue;
            }
            PSX_LOGE("accept: %s", strerror(errno));
            return;
        }

        psx_set_cloexec(fd, true);
        psx_set_nonblocking(fd, true);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if(psx_session_manager_count(&server->sessions) >=
           server->config->max_sessions) {
            PSX_LOGW("connection rejected: session limit reached (%zu)",
                     server->config->max_sessions);
            send_hello_ack(fd, PTTY_ACK_SERVER_BUSY);
            close(fd);
            continue;
        }

        if(!(session = psx_session_manager_create(&server->sessions, fd))) {
            PSX_LOGE("cannot create session: %s", strerror(errno));
            close(fd);
            continue;
        }

        session->handshake_deadline_ms =
            psx_now_ms() + (uint64_t)server->config->handshake_timeout_ms;
        PSX_LOGD("session %u: accepted", session->id);
    }
}

static void
server_tick(psx_server_t *server)
{
    uint64_t now = psx_now_ms();
    psx_session_t *session;
    psx_session_t *next;

    for(session = server->sessions.sessions; session; session = next) {
        next = session->next;

        /* A client asked for an administrative shutdown. */
        if(session->shutdown_requested) {
            PSX_NOTIFY("shutdown requested by a client");
            g_shutdown = 1;
            break;
        }

        /* Placeholder sessions left over from a successful attach. */
        if(session->state == PSX_SESSION_CLOSED) {
            server_session_remove(server, session, "closed");
            continue;
        }

        /* Finished foreground processes. */
        psx_session_check_process(session);

        /* Shell requested exit (attached or detached). */
        if((session->state == PSX_SESSION_RUNNING ||
            session->state == PSX_SESSION_DETACHED) &&
           session->shell && psh_shell_exit_requested(session->shell)) {
            server_session_start_closing(session);
        }

        /* Unclaimed detached session: reclaim it after the timeout. */
        if(session->state == PSX_SESSION_DETACHED &&
           server->config->detached_timeout_ms > 0 &&
           now - session->detached_since_ms >
               (uint64_t)server->config->detached_timeout_ms) {
            server_session_remove(server, session, "detached timeout");
            continue;
        }

        /* Closing: flush pending output, then drop. */
        if(session->state == PSX_SESSION_CLOSING) {
            if(psx_session_flush(session) < 0 ||
               psx_buf_pending(&session->out) == 0 ||
               now >= session->close_deadline_ms) {
                server_session_remove(server, session, "closed");
                continue;
            }
        }

        /* Handshake timeout. */
        if((session->state == PSX_SESSION_ACCEPTED ||
            session->state == PSX_SESSION_HANDSHAKING) &&
           session->handshake_deadline_ms &&
           now >= session->handshake_deadline_ms) {
            server_session_remove(server, session, "handshake timeout");
            continue;
        }

        /* Optional idle timeout. */
        if(server->config->idle_timeout_ms > 0 &&
           session->state == PSX_SESSION_RUNNING &&
           now - session->last_activity_ms >
               (uint64_t)server->config->idle_timeout_ms) {
            server_session_remove(server, session, "idle timeout");
            continue;
        }
    }
}

static void
server_handle_error(psx_server_t *server, psx_session_t *session,
                    const char *what)
{
    server_session_remove(server, session, what);
}

static int
server_loop(psx_server_t *server)
{
    size_t cap = 2 + server->config->max_sessions * 3;

    server->pfds = calloc(cap, sizeof(struct pollfd));
    server->owners = calloc(cap, sizeof(uint32_t));

    if(!server->pfds || !server->owners) {
        PSX_LOGE("out of memory");
        return -1;
    }

    server->pfds_cap = cap;

    while(!g_shutdown) {
        size_t n = 0;
        int rc;

        server->pfds[n].fd = server->listen_fd;
        server->pfds[n].events = POLLIN;
        server->owners[n] = 0;
        n++;

        server->pfds[n].fd = server->signal_pipe[0];
        server->pfds[n].events = POLLIN;
        server->owners[n] = 0;
        n++;

        for(psx_session_t *s = server->sessions.sessions; s; s = s->next) {
            /* Three entries per session at most: socket, tty, stderr. */
            if(s->state == PSX_SESSION_CLOSED || n + 3 > server->pfds_cap) {
                continue;
            }

            if(s->sock_fd >= 0) {
                server->pfds[n].fd = s->sock_fd;
                server->pfds[n].events = 0;
                /* Backpressure: stop reading while the tty input queue is
                 * full. */
                if(psx_buf_pending(&s->in) < PSX_SESSION_IN_HIGH_WATER) {
                    server->pfds[n].events |= POLLIN;
                }
                if(psx_buf_pending(&s->out) > 0) {
                    server->pfds[n].events |= POLLOUT;
                }
                server->owners[n] = s->id;
                n++;
            }

            /* Detached sessions keep draining their tty into the bounded
             * scrollback so a foreground process is never blocked by a
             * missing client. */
            if(s->tty.master_fd >= 0 &&
               (s->state == PSX_SESSION_RUNNING ||
                s->state == PSX_SESSION_DETACHED)) {
                server->pfds[n].fd = s->tty.master_fd;
                server->pfds[n].events = POLLIN;
                if(psx_buf_pending(&s->in) > 0) {
                    server->pfds[n].events |= POLLOUT;
                }
                server->owners[n] = s->id;
                n++;
            }

            if(s->proc.stderr_fd >= 0 &&
               (s->state == PSX_SESSION_RUNNING ||
                s->state == PSX_SESSION_DETACHED)) {
                server->pfds[n].fd = s->proc.stderr_fd;
                server->pfds[n].events = POLLIN;
                server->owners[n] = s->id;
                n++;
            }
        }

        rc = poll(server->pfds, (nfds_t)n, SERVER_TICK_MS);

        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            PSX_LOGE("poll: %s", strerror(errno));
            return -1;
        }

        if(rc > 0) {
            for(size_t i = 0; i < n; i++) {
                short revents = server->pfds[i].revents;
                psx_session_t *session;

                if(revents == 0) {
                    continue;
                }

                if(i == 0) {
                    if(revents & POLLIN) {
                        server_accept(server);
                    }
                    continue;
                }

                if(i == 1) {
                    if(revents & POLLIN) {
                        uint8_t drain[32];
                        ssize_t got = read(server->signal_pipe[0], drain,
                                           sizeof(drain));

                        if(got > 0) {
                            PSX_NOTIFY("shutdown requested (signal %u)",
                                       (unsigned)drain[0]);
                        }
                        g_shutdown = 1;
                    }
                    continue;
                }

                if(!(session = psx_session_manager_find(&server->sessions,
                                                        server->owners[i]))) {
                    continue;
                }

                if(session->tty.master_fd >= 0 &&
                   server->pfds[i].fd == session->tty.master_fd) {
                    if(revents & (POLLIN | POLLHUP | POLLERR)) {
                        if(psx_session_on_tty_readable(session) < 0) {
                            server_handle_error(server, session, "tty error");
                            continue;
                        }
                    }
                    if(revents & POLLOUT) {
                        if(psx_session_flush_tty_input(session) < 0) {
                            server_handle_error(server, session, "tty error");
                            continue;
                        }
                    }
                    continue;
                }

                if(session->proc.stderr_fd >= 0 &&
                   server->pfds[i].fd == session->proc.stderr_fd) {
                    if(revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                        if(psx_session_on_stderr_readable(session) < 0) {
                            server_handle_error(server, session, "stderr error");
                        }
                    }
                    continue;
                }

                /* Stale entry: the session detached or was replaced. */
                if(session->sock_fd < 0 ||
                   server->pfds[i].fd != session->sock_fd) {
                    continue;
                }

                if(revents & (POLLIN | POLLHUP | POLLERR)) {
                    int rc = server_session_readable(server, session);

                    if(rc == PSX_SESSION_FRAME_ERROR) {
                        server_handle_error(server, session, "protocol error");
                        continue;
                    }
                    if(rc == PSX_SESSION_FRAME_CLOSE) {
                        server_session_start_closing(session);
                        continue;
                    }
                    if(rc == PSX_SESSION_FRAME_REPLACED) {
                        server_session_remove(server, session,
                                              "replaced by attach");
                        continue;
                    }
                    if(rc == PSX_SESSION_FRAME_DETACH) {
                        if(server->config->persist_sessions &&
                           session->state == PSX_SESSION_RUNNING) {
                            psx_session_detach(session);
                        } else {
                            server_session_remove(server, session,
                                                  "client disconnected");
                        }
                        continue;
                    }
                }

                if(revents & POLLOUT) {
                    if(psx_session_flush(session) < 0) {
                        server_handle_error(server, session, "socket error");
                        continue;
                    }
                }
            }
        }

        server_tick(server);
    }

    return 0;
}

/* --- listen socket ------------------------------------------------------ */

static int
server_listen(psx_server_t *server)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char port[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    snprintf(port, sizeof(port), "%u", (unsigned)server->config->port);

    if(getaddrinfo(server->config->bind_addr, port, &hints, &result) != 0) {
        PSX_LOGE("cannot resolve %s:%s", server->config->bind_addr, port);
        return -1;
    }

    for(ai = result; ai; ai = ai->ai_next) {
        int one = 1;

        if((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if(bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
           listen(fd, SERVER_BACKLOG) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if(fd < 0) {
        PSX_LOGE("cannot listen on %s:%u: %s", server->config->bind_addr,
                 (unsigned)server->config->port, strerror(errno));
        return -1;
    }

    psx_set_cloexec(fd, true);
    psx_set_nonblocking(fd, true);
    server->listen_fd = fd;

    return 0;
}

int
psx_server_run(const psx_server_config_t *config)
{
    psx_server_t server;
    int rc = 0;

    memset(&server, 0, sizeof(server));
    server.config = config;
    server.listen_fd = -1;
    server.signal_pipe[0] = server.signal_pipe[1] = -1;

    psx_session_manager_init(&server.sessions, config->max_sessions);

    if(pipe(server.signal_pipe) < 0) {
        PSX_LOGE("signal pipe: %s", strerror(errno));
        return -1;
    }
    g_signal_pipe[0] = server.signal_pipe[0];
    g_signal_pipe[1] = server.signal_pipe[1];
    psx_set_nonblocking(server.signal_pipe[1], true);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Replace a previously running daemon before taking the port. */
    psx_instance_claim();

    if(server_listen(&server) < 0) {
        /*
         * The port may still be held by an instance that predates the pid
         * file (first upgrade on a console). Replace same-name siblings and
         * retry, so a console reboot is never required.
         */
        if(errno == EADDRINUSE) {
            PSX_LOGW("port %u is busy; looking for a previous PSXTerm instance",
                     (unsigned)config->port);

            if(psx_instance_replace_siblings() > 0) {
                psx_platform_notify("PSXTERM: replaced the previous instance");
                for(int attempt = 0; attempt < 15; attempt++) {
                    struct timespec pause = {.tv_sec = 0,
                                             .tv_nsec = 200 * 1000 * 1000};

                    nanosleep(&pause, NULL);

                    if(server_listen(&server) == 0) {
                        PSX_LOGI("port %u acquired after replacing the "
                                 "previous instance",
                                 (unsigned)config->port);
                        break;
                    }
                    if(errno != EADDRINUSE) {
                        break;
                    }
                }
            }
        }

        if(server.listen_fd < 0) {
            rc = -1;
            goto out;
        }
    }

    PSX_LOGI("PSXTerm %s (%s) listening on %s:%u", PSXTERM_VERSION_STRING,
             psx_platform_name(), config->bind_addr, (unsigned)config->port);
    PSX_NOTIFY("started on port %u (%s, protocol %s, version %s)",
               (unsigned)config->port, psx_platform_name(),
               PSXTERM_PROTOCOL_NAME, PSXTERM_VERSION_STRING);

    {
        char toast[128];

        snprintf(toast, sizeof(toast), "PSXTERM: started on port %u",
                 (unsigned)config->port);
        psx_platform_notify(toast);
    }

    if(config->auth_mode == PSX_AUTH_TOKEN) {
        PSX_NOTIFY("authentication: shared token required");
    } else {
        PSX_LOGW("authentication: DISABLED (insecure development mode) - "
                 "do not expose this port to untrusted networks");
    }

    rc = server_loop(&server);

out:
    for(psx_session_t *s = server.sessions.sessions; s; s = s->next) {
        psx_session_emit(s, PTTY_MSG_CLOSE, NULL, 0);
        psx_session_flush(s);
    }

    while(server.sessions.sessions) {
        psx_session_manager_remove(&server.sessions, server.sessions.sessions);
    }

    if(server.listen_fd >= 0) {
        close(server.listen_fd);
    }

    psx_instance_release();
    if(server.signal_pipe[0] >= 0) {
        close(server.signal_pipe[0]);
        close(server.signal_pipe[1]);
    }
    g_signal_pipe[0] = g_signal_pipe[1] = -1;

    free(server.pfds);
    free(server.owners);

    return rc;
}

/* --- entry point -------------------------------------------------------- */

static void
usage(const char *argv0)
{
    printf("usage: %s [options]\n", argv0);
    printf("\n");
    printf("PSXTerm daemon - remote terminal server for PS4/PS5 payloads.\n");
    printf("\n");
    printf("  -p, --port PORT          TCP port (default %d)\n",
           PSXTERM_DEFAULT_PORT);
    printf("      --bind ADDR          bind address (default 0.0.0.0)\n");
    printf("      --max-sessions N     session limit (default %d)\n",
           PSXTERM_MAX_SESSIONS_DEFAULT);
    printf("      --token TOKEN        require this shared token\n");
    printf("      --handshake-timeout MS  handshake deadline (default %d)\n",
           SERVER_DEFAULT_HANDSHAKE_MS);
    printf("      --idle-timeout MS    drop idle sessions (default off)\n");
    printf("      --no-persist         destroy sessions on disconnect\n");
    printf("      --detached-timeout MS  reclaim detached sessions "
           "(default %d)\n",
           30 * 60 * 1000);
    printf("      --tty MODE           auto|pty|pipe (default auto)\n");
    printf("      --doctor             run local diagnostics and exit\n");
    printf("      --json               machine-readable diagnostics output\n");
    printf("  -v, --verbose            debug logging\n");
    printf("  -q, --quiet              errors only\n");
    printf("      --version            print version\n");
    printf("  -h, --help               this help\n");
}

static int
parse_args(int argc, char **argv, psx_server_config_t *config, bool *doctor,
           bool *json)
{
    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if(strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            if(++i >= argc || !psx_parse_u16(argv[i], &config->port) ||
               config->port == 0) {
                PSX_LOGE("invalid port");
                return -1;
            }
        } else if(strcmp(arg, "--bind") == 0) {
            if(++i >= argc) {
                return -1;
            }
            config->bind_addr = argv[i];
        } else if(strcmp(arg, "--max-sessions") == 0) {
            uint32_t value;

            if(++i >= argc || !psx_parse_u32(argv[i], &value) || value == 0 ||
               value > 256) {
                PSX_LOGE("invalid session limit");
                return -1;
            }
            config->max_sessions = value;
        } else if(strcmp(arg, "--token") == 0) {
            if(++i >= argc || strlen(argv[i]) >= sizeof(config->auth_token)) {
                PSX_LOGE("invalid token");
                return -1;
            }
            snprintf(config->auth_token, sizeof(config->auth_token), "%s",
                     argv[i]);
            config->auth_mode = PSX_AUTH_TOKEN;
        } else if(strcmp(arg, "--handshake-timeout") == 0) {
            int value;

            if(++i >= argc || !psx_parse_int(argv[i], &value) || value < 100) {
                PSX_LOGE("invalid handshake timeout");
                return -1;
            }
            config->handshake_timeout_ms = value;
        } else if(strcmp(arg, "--idle-timeout") == 0) {
            int value;

            if(++i >= argc || !psx_parse_int(argv[i], &value) || value < 0) {
                PSX_LOGE("invalid idle timeout");
                return -1;
            }
            config->idle_timeout_ms = value;
        } else if(strcmp(arg, "--no-persist") == 0) {
            config->persist_sessions = false;
        } else if(strcmp(arg, "--detached-timeout") == 0) {
            int value;

            if(++i >= argc || !psx_parse_int(argv[i], &value) || value < 0) {
                PSX_LOGE("invalid detached timeout");
                return -1;
            }
            config->detached_timeout_ms = value;
        } else if(strcmp(arg, "--tty") == 0) {
            if(++i >= argc) {
                return -1;
            }
            if(strcmp(argv[i], "auto") == 0) {
                psx_tty_force_backend(PSX_TTY_BACKEND_NONE);
            } else if(strcmp(argv[i], "pty") == 0) {
                psx_tty_force_backend(PSX_TTY_BACKEND_FREEBSD_PTY);
            } else if(strcmp(argv[i], "pipe") == 0) {
                psx_tty_force_backend(PSX_TTY_BACKEND_PIPE);
            } else {
                PSX_LOGE("invalid tty backend: %s (want auto|pty|pipe)",
                         argv[i]);
                return -1;
            }
        } else if(strcmp(arg, "--doctor") == 0) {
            *doctor = true;
        } else if(strcmp(arg, "--json") == 0) {
            *json = true;
        } else if(strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            psx_log_set_level(PSX_LOG_DEBUG);
        } else if(strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            psx_log_set_level(PSX_LOG_ERROR);
        } else if(strcmp(arg, "--version") == 0) {
            printf("psxtermd %s (protocol %s, platform %s)\n",
                   PSXTERM_VERSION_STRING, PSXTERM_PROTOCOL_NAME,
                   psx_platform_name());
            exit(0);
        } else if(strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            PSX_LOGE("unknown option: %s", arg);
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/* Local (on-console) diagnostics: no client session required. */
static int
run_local_diagnostics(bool json)
{
    psx_diag_report_t report;
    psx_diag_status_t overall;
    size_t needed;
    char *text;

    psx_diag_report_init(&report);
    psx_diag_run(&report, NULL);

    needed = json ? psx_diag_format_json(&report, NULL, 0)
                  : psx_diag_format_human(&report, NULL, 0);

    if(!(text = malloc(needed + 1))) {
        psx_diag_report_free(&report);
        return PSX_DIAG_EXIT_FAILED;
    }

    if(json) {
        psx_diag_format_json(&report, text, needed + 1);
    } else {
        psx_diag_format_human(&report, text, needed + 1);
    }

    fputs(text, stdout);
    fflush(stdout);
    free(text);

    overall = psx_diag_overall_status(&report);
    psx_diag_report_free(&report);

    return psx_diag_exit_code(overall);
}

int
main(int argc, char **argv)
{
    psx_server_config_t config;
    bool doctor = false;
    bool json = false;

    psx_server_config_default(&config);

    if(parse_args(argc, argv, &config, &doctor, &json) < 0) {
        return 2;
    }

    if(!doctor) {
        psx_log(PSX_LOG_INFO, "PSXTERM by SeregonWar started");
    }

    if(!psx_platform_init()) {
        PSX_LOGE("platform initialization failed");
        return 1;
    }

    if(!doctor) {
        /* The console's own toast, so the operator sees the daemon start
         * without watching a log. */
        psx_platform_notify("PSXTERM by SeregonWar started");
    }

    /*
     * The runtime layout is what external CLI processes expect: home, tmp,
     * XDG locations, certificate bundle. Create it before serving, but never
     * refuse to start because of it.
     */
    if(!doctor) {
        if(!psx_runtime_prepare()) {
            PSX_LOGW("runtime layout incomplete; external CLIs may misbehave");
        }
        if(!psx_runtime_manifest_write(NULL)) {
            PSX_LOGW("runtime manifest unavailable; package metadata missing");
        }
    }

    if(doctor) {
        return run_local_diagnostics(json);
    }

    return psx_server_run(&config) == 0 ? 0 : 1;
}
