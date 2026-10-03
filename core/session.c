#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psxterm/diag.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/protocol.h"
#include "psxterm/runtime.h"
#include "psxterm/session.h"
#include "psxterm/shell.h"
#include "psxterm/tty.h"

/* Bound on queued client output: a session that stops reading must not make
 * the daemon grow without bound. */
#define PSX_SESSION_OUT_MAX (8u * 1024u * 1024u)

#define PSX_EMIT_CHUNK PTTY_MAX_PAYLOAD

static void file_transfer_reset(psx_session_t *session, bool remove_temp);
static int file_handle_read_request(psx_session_t *session);

psx_session_t *
psx_session_create(uint32_t id, int sock_fd)
{
    psx_session_t *session = calloc(1, sizeof(*session));

    if(!session) {
        return NULL;
    }

    session->id = id;
    session->sock_fd = sock_fd;
    session->file_fd = -1;
    session->proc.stdin_fd = -1;
    session->proc.relay_out = -1;
    session->proc.relay_in = -1;
    session->proc.loader = false;
    session->proc.exit_code_hint = 0;
    session->proc.exit_reported = false;
    session->proc.stderr_fd = -1;
    session->state = PSX_SESSION_ACCEPTED;
    session->rows = 24;
    session->cols = 80;
    session->created_ms = session->last_activity_ms = psx_now_ms();

    psx_tty_init(&session->tty);
    psx_buf_init(&session->out);
    psx_buf_init(&session->in);
    ptty_reader_init(&session->reader, sock_fd);

    snprintf(session->command, sizeof(session->command), "%s", "psh");

    if(psx_platform_random_bytes(session->resume_token,
                                 sizeof(session->resume_token)) < 0) {
        /* Falls back to a time/PID mix: still unpredictable enough to avoid
         * a predictable token, and never logged either way. */
        uint64_t seed = psx_now_ms() ^ ((uint64_t)getpid() << 32);

        for(size_t i = 0; i < sizeof(session->resume_token); i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            session->resume_token[i] = (uint8_t)(seed >> 33);
        }
    }

    return session;
}

void
psx_session_destroy(psx_session_t *session)
{
    if(!session) {
        return;
    }

    if(session->proc.running && session->proc.pid > 0) {
        int status;

        psx_process_kill(session->proc.pid, SIGTERM);
        for(int i = 0; i < 20; i++) {
            if(psx_process_wait(session->proc.pid, &status, 50) == 0) {
                break;
            }
        }
        if(psx_process_wait(session->proc.pid, &status, 0) == 1) {
            psx_process_kill(session->proc.pid, SIGKILL);
            psx_process_wait(session->proc.pid, &status, 200);
        }
        session->proc.running = false;
    }

    if(session->proc.stdin_fd >= 0) {
        close(session->proc.stdin_fd);
    }
    if(session->proc.stderr_fd >= 0) {
        close(session->proc.stderr_fd);
    }

    psh_shell_destroy(session->shell);
    psx_tty_close(&session->tty);
    ptty_reader_destroy(&session->reader);
    psx_buf_free(&session->out);
    psx_buf_free(&session->in);
    psx_env_clear(&session->env);
    free(session->scrollback);
    file_transfer_reset(session, true);

    if(session->sock_fd >= 0) {
        close(session->sock_fd);
    }

    session->state = PSX_SESSION_CLOSED;
    free(session);
}

int
psx_session_emit(psx_session_t *session, uint8_t type, const void *data, size_t len)
{
    size_t offset = 0;

    /*
     * A detached session has no socket: terminal output goes into the bounded
     * scrollback instead of a per-client queue. Control frames (EXIT/CLOSE/
     * CAPS/...) have no client to reach and are dropped.
     */
    if(session->sock_fd < 0) {
        if((type == PTTY_MSG_STDOUT || type == PTTY_MSG_STDERR) && data) {
            psx_session_scrollback_append(session, data, len);
        }
        return 0;
    }

    if(psx_buf_pending(&session->out) > PSX_SESSION_OUT_MAX) {
        errno = ENOBUFS;
        return -1;
    }

    session->frames_out++;

    do {
        size_t chunk = len - offset;
        ptty_header_t header = {
            .magic = PTTY_MAGIC,
            .version = PTTY_VERSION,
            .type = type,
            .flags = 0,
            .session_id = session->id,
            .payload_length = (uint32_t)chunk,
        };

        if(chunk > PSX_EMIT_CHUNK) {
            chunk = PSX_EMIT_CHUNK;
            header.payload_length = (uint32_t)chunk;
        }

        /*
         * The bound is re-checked for every chunk so a single large emit can
         * never overshoot the queue limit by more than one frame.
         */
        if(psx_buf_pending(&session->out) + PTTY_HEADER_SIZE + chunk >
           PSX_SESSION_OUT_MAX) {
            errno = ENOBUFS;
            return -1;
        }

        if(ptty_queue_frame(&session->out, &header,
                            chunk ? (const uint8_t *)data + offset : NULL) < 0) {
            return -1;
        }

        offset += chunk;
    } while(offset < len);

    return 0;
}

int
psx_session_emit_fmt(psx_session_t *session, uint8_t type, const char *fmt, ...)
{
    char message[1024];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if(len < 0) {
        return -1;
    }
    if((size_t)len >= sizeof(message)) {
        len = (int)sizeof(message) - 1;
    }

    return psx_session_emit(session, type, message, (size_t)len);
}

int
psx_session_flush(psx_session_t *session)
{
    if(psx_buf_pending(&session->out) == 0) {
        return 0;
    }

    if(psx_buf_flush(&session->out, session->sock_fd) < 0) {
        return -1;
    }

    return 0;
}

void
psx_session_touch(psx_session_t *session)
{
    session->last_activity_ms = psx_now_ms();
}

bool
psx_session_has_process(const psx_session_t *session)
{
    return session->proc.running;
}

const char *
psx_session_state_name(psx_session_state_t state)
{
    switch(state) {
    case PSX_SESSION_ACCEPTED: return "accepted";
    case PSX_SESSION_HANDSHAKING: return "handshake";
    case PSX_SESSION_RUNNING: return "running";
    case PSX_SESSION_DETACHED: return "detached";
    case PSX_SESSION_CLOSING: return "closing";
    case PSX_SESSION_CLOSED: return "closed";
    default: return "unknown";
    }
}

/* --- session persistence ------------------------------------------------ */

void
psx_session_scrollback_append(psx_session_t *session, const uint8_t *data,
                              size_t len)
{
    size_t tail;
    size_t first;

    if(len == 0) {
        return;
    }

    if(!session->scrollback) {
        if(!(session->scrollback = malloc(PSX_SESSION_SCROLLBACK_MAX))) {
            return;
        }
        session->scrollback_cap = PSX_SESSION_SCROLLBACK_MAX;
        session->scrollback_len = 0;
        session->scrollback_start = 0;
    }

    if(len >= session->scrollback_cap) {
        /* Keep only the newest bytes. */
        memcpy(session->scrollback, data + (len - session->scrollback_cap),
               session->scrollback_cap);
        session->scrollback_start = 0;
        session->scrollback_len = session->scrollback_cap;
        session->scrollback_truncated = true;
        return;
    }

    if(session->scrollback_len + len > session->scrollback_cap) {
        size_t drop = session->scrollback_len + len - session->scrollback_cap;

        session->scrollback_start =
            (session->scrollback_start + drop) % session->scrollback_cap;
        session->scrollback_len -= drop;
        session->scrollback_truncated = true;
    }

    tail = (session->scrollback_start + session->scrollback_len) %
           session->scrollback_cap;
    first = session->scrollback_cap - tail;
    if(first > len) {
        first = len;
    }

    memcpy(session->scrollback + tail, data, first);
    if(len > first) {
        memcpy(session->scrollback, data + first, len - first);
    }

    session->scrollback_len += len;
}

static void
session_flush_scrollback(psx_session_t *session)
{
    if(session->scrollback_truncated) {
        static const char notice[] =
            "[PSXTerm: output truncated while detached]\r\n";

        psx_session_emit(session, PTTY_MSG_STDOUT, notice,
                         sizeof(notice) - 1);
        session->scrollback_truncated = false;
    }

    if(!session->scrollback || session->scrollback_len == 0) {
        return;
    }

    {
        size_t first = session->scrollback_cap - session->scrollback_start;

        if(first > session->scrollback_len) {
            first = session->scrollback_len;
        }

        psx_session_emit(session, PTTY_MSG_STDOUT,
                         session->scrollback + session->scrollback_start,
                         first);

        if(session->scrollback_len > first) {
            psx_session_emit(session, PTTY_MSG_STDOUT, session->scrollback,
                             session->scrollback_len - first);
        }
    }

    session->scrollback_len = 0;
    session->scrollback_start = 0;
}

bool
psx_session_check_token(const psx_session_t *session, const uint8_t *token,
                        size_t len)
{
    unsigned diff = len != sizeof(session->resume_token);

    for(size_t i = 0; i < len && i < sizeof(session->resume_token); i++) {
        diff |= (unsigned)(session->resume_token[i] ^ token[i]);
    }

    return diff == 0;
}

bool
psx_session_is_detached(const psx_session_t *session)
{
    return session->state == PSX_SESSION_DETACHED;
}

void
psx_session_detach(psx_session_t *session)
{
    if(session->state != PSX_SESSION_RUNNING) {
        return;
    }

    /*
     * An interrupted transfer must not leave a temporary file behind: the
     * client is gone and cannot finish the upload.
     */
    file_transfer_reset(session, true);

    /* Output queued for the departed client is lost; say so honestly. */
    if(psx_buf_pending(&session->out) > 0) {
        session->scrollback_truncated = true;
    }
    psx_buf_free(&session->out);
    psx_buf_init(&session->out);

    if(session->sock_fd >= 0) {
        close(session->sock_fd);
        session->sock_fd = -1;
    }

    ptty_reader_destroy(&session->reader);
    ptty_reader_init(&session->reader, -1);

    session->state = PSX_SESSION_DETACHED;
    session->detached_since_ms = psx_now_ms();

    PSX_LOGI("session %u detached (idle for reattach, no token logged)",
             session->id);
}

int
psx_session_attach(psx_session_t *session, int sock_fd)
{
    if(session->state != PSX_SESSION_DETACHED) {
        errno = EBUSY;
        return -1;
    }

    psx_set_cloexec(sock_fd, true);
    psx_set_nonblocking(sock_fd, true);

    session->sock_fd = sock_fd;
    ptty_reader_destroy(&session->reader);
    ptty_reader_init(&session->reader, sock_fd);

    session->state = PSX_SESSION_RUNNING;
    session->last_activity_ms = psx_now_ms();
    session->frames_in = 0;
    session->frames_out = 0;
    session->protocol_errors = 0;

    /* Refresh the capability advertisement for the new connection. */
    session->capabilities = psx_session_capabilities(session);
    {
        uint8_t payload[4] = {
            (uint8_t)(session->capabilities & 0xff),
            (uint8_t)((session->capabilities >> 8) & 0xff),
            (uint8_t)((session->capabilities >> 16) & 0xff),
            (uint8_t)((session->capabilities >> 24) & 0xff),
        };

        psx_session_emit(session, PTTY_MSG_CAPS, payload, sizeof(payload));
    }

    session_flush_scrollback(session);

    if(session->shell && !session->proc.running) {
        psh_shell_prompt(session->shell);
    }

    PSX_LOGI("session %u reattached", session->id);

    return 0;
}

/* --- path handling ------------------------------------------------------ */

static int
normalize_path(const char *in, char *out, size_t out_cap)
{
    char work[PSX_PATH_MAX];
    const char *segments[PSX_PATH_MAX / 2];
    size_t count = 0;
    size_t used = 0;
    char *saveptr = NULL;

    if(strlen(in) >= sizeof(work)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(work, in);

    for(char *token = strtok_r(work, "/", &saveptr); token;
        token = strtok_r(NULL, "/", &saveptr)) {
        if(strcmp(token, ".") == 0) {
            continue;
        }
        if(strcmp(token, "..") == 0) {
            if(count > 0) {
                count--;
            }
            continue;
        }
        segments[count++] = token;
    }

    if(out_cap < 2) {
        errno = ENAMETOOLONG;
        return -1;
    }

    out[used++] = '/';
    for(size_t i = 0; i < count; i++) {
        size_t seg_len = strlen(segments[i]);

        if(used + seg_len + 2 > out_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }

        if(i > 0) {
            out[used++] = '/';
        }
        memcpy(out + used, segments[i], seg_len);
        used += seg_len;
    }
    out[used] = '\0';

    return 0;
}

int
psx_session_absolute_path(const psx_session_t *session, const char *path,
                          char *out, size_t out_cap)
{
    char combined[PSX_PATH_MAX];

    if(!path || !*path) {
        errno = EINVAL;
        return -1;
    }

    if(path[0] == '/') {
        if(strlen(path) >= sizeof(combined)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(combined, path);
    } else if(path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        const char *home = psx_env_get(&session->env, "HOME");

        if(!home || !*home) {
            errno = ENOENT;
            return -1;
        }
        if(psx_path_join(combined, sizeof(combined), home, path + 1) < 0) {
            return -1;
        }
    } else {
        if(psx_path_join(combined, sizeof(combined), session->cwd, path) < 0) {
            return -1;
        }
    }

    return normalize_path(combined, out, out_cap);
}

static bool
is_executable_file(const char *path)
{
    struct stat st;

    if(stat(path, &st) != 0) {
        return false;
    }

    return S_ISREG(st.st_mode);
}

char *
psx_session_resolve_path(psx_session_t *session, const char *name)
{
    const char *path_env;
    char candidate[PSX_PATH_MAX];
    char *saveptr = NULL;
    char *dirs;

    if(strchr(name, '/')) {
        char resolved[PSX_PATH_MAX];

        if(psx_session_absolute_path(session, name, resolved, sizeof(resolved)) < 0) {
            return NULL;
        }

        return is_executable_file(resolved) ? strdup(resolved) : NULL;
    }

    path_env = psx_env_get(&session->env, "PATH");
    if(!path_env || !*path_env) {
        path_env = psx_platform_default_path();
    }

    if(!(dirs = strdup(path_env))) {
        return NULL;
    }

    for(char *dir = strtok_r(dirs, ":", &saveptr); dir;
        dir = strtok_r(NULL, ":", &saveptr)) {
        char with_ext[PSX_PATH_MAX];
        size_t name_len = strlen(name);

        if(!*dir) {
            continue;
        }

        if(psx_path_join(candidate, sizeof(candidate), dir, name) == 0 &&
           is_executable_file(candidate)) {
            free(dirs);
            return strdup(candidate);
        }

        if(name_len + sizeof(".elf") > sizeof(with_ext)) {
            continue;
        }
        memcpy(with_ext, name, name_len);
        memcpy(with_ext + name_len, ".elf", sizeof(".elf"));

        if(psx_path_join(candidate, sizeof(candidate), dir, with_ext) == 0 &&
           is_executable_file(candidate)) {
            free(dirs);
            return strdup(candidate);
        }
    }

    free(dirs);

    return NULL;
}

/* --- session start ------------------------------------------------------ */

uint32_t
psx_session_capabilities(const psx_session_t *session)
{
    uint32_t caps = 0;

    if(session->tty.backend == PSX_TTY_BACKEND_FREEBSD_PTY) {
        caps |= PTTY_CAP_REAL_PTY;
    } else if(session->tty.backend == PSX_TTY_BACKEND_PIPE) {
        caps |= PTTY_CAP_PIPE_TTY;
    }

    /* Only advertise what this process can actually do right now. */
    if(psx_process_backend_available()) {
        caps |= PTTY_CAP_EXEC;
    }

    /* Job control needs terminal semantics; never faked on PipeTTY. */
    if(session->tty.is_real_pty) {
        caps |= PTTY_CAP_JOB_CONTROL;
    }

    caps |= PTTY_CAP_JSON_DIAGNOSTICS;

    /* Persistence and chunked file transfer are part of this build. */
    caps |= PTTY_CAP_SESSION_RESUME;
    caps |= PTTY_CAP_FILE_TRANSFER;

    return caps;
}

/*
 * Give external CLI processes the environment the runtime layout promises:
 * the runtime home, its temporary directory, the XDG locations and the
 * certificate bundle. Contract keys always win over whatever the daemon
 * inherited; PATH keeps the inherited entries after the runtime ones, and
 * the client's terminal type is left alone.
 */
static void
session_apply_runtime_env(psx_session_t *session)
{
    psx_runtime_env_t rt[16];
    size_t count = psx_runtime_env_table(rt, 16);

    for(size_t i = 0; i < count; i++) {
        const char *existing;

        if(strcmp(rt[i].key, "PATH") == 0) {
            char merged[PSX_PATH_MAX * 2 + 2];

            existing = psx_env_get(&session->env, "PATH");
            if(existing && *existing) {
                snprintf(merged, sizeof(merged), "%s:%s", rt[i].value,
                         existing);
            } else {
                snprintf(merged, sizeof(merged), "%s", rt[i].value);
            }
            psx_env_set(&session->env, "PATH", merged);
            continue;
        }

        if(strcmp(rt[i].key, "TERM") == 0) {
            continue;
        }

        existing = psx_env_get(&session->env, rt[i].key);
        if(psx_runtime_env_is_contract(rt[i].key) || existing == NULL ||
           !*existing) {
            psx_env_set(&session->env, rt[i].key, rt[i].value);
        }
    }
}

int
psx_session_begin(psx_session_t *session)
{
    psx_tty_backend_t backend = psx_tty_default_backend();

    psx_env_init(&session->env, psx_platform_inherit_environ());

    if(session->term[0]) {
        psx_env_set(&session->env, "TERM", session->term);
    }

    if(!getcwd(session->cwd, sizeof(session->cwd))) {
        snprintf(session->cwd, sizeof(session->cwd), "%s", psx_fs_default_cwd());
    }
    psx_env_set(&session->env, "PWD", session->cwd);

    session_apply_runtime_env(session);

    if(psx_tty_open(&session->tty, backend, session->rows, session->cols) < 0) {
        PSX_LOGW("session %u: tty backend %s unavailable (%s)", session->id,
                 psx_tty_backend_name(backend), strerror(errno));

        if(backend != PSX_TTY_BACKEND_PIPE &&
           psx_tty_open(&session->tty, PSX_TTY_BACKEND_PIPE, session->rows,
                        session->cols) < 0) {
            PSX_LOGE("session %u: no tty backend available (%s)", session->id,
                     strerror(errno));
        }
    }

    if(!(session->shell = psh_shell_create(session))) {
        return -1;
    }

    session->state = PSX_SESSION_RUNNING;

    /*
     * Advertise runtime capabilities right after OPEN_OK (queued by the
     * server before this call). Clients that do not know the frame ignore it.
     */
    session->capabilities = psx_session_capabilities(session);
    {
        uint8_t payload[4] = {
            (uint8_t)(session->capabilities & 0xff),
            (uint8_t)((session->capabilities >> 8) & 0xff),
            (uint8_t)((session->capabilities >> 16) & 0xff),
            (uint8_t)((session->capabilities >> 24) & 0xff),
        };

        psx_session_emit(session, PTTY_MSG_CAPS, payload, sizeof(payload));
    }

    psh_shell_prompt(session->shell);

    return 0;
}

/* --- process handling --------------------------------------------------- */

/*
 * Build the one-line command the wrapper payload reads, from the resolved
 * path and its arguments. Arguments with spaces or quotes are quoted, because
 * that line is split on spaces on the other side.
 */
static int
session_build_command_line(const char *path, char *const *argv, char *out,
                           size_t cap)
{
    size_t used = 0;
    int written = snprintf(out, cap, "%s", path);

    if(written < 0 || (size_t)written >= cap) {
        return -1;
    }
    used = (size_t)written;

    for(size_t i = 1; argv && argv[i]; i++) {
        bool quote = strpbrk(argv[i], " \t\"'") != NULL;

        written = snprintf(out + used, cap - used, " %s%s%s",
                           quote ? "\"" : "", argv[i], quote ? "\"" : "");

        if(written < 0 || (size_t)written >= cap - used) {
            return -1;
        }
        used += (size_t)written;
    }

    return (int)used;
}

int
psx_session_spawn_process(psx_session_t *session, const char *path,
                          char *const *argv)
{
    psx_spawn_options_t options;
    char *envp[PSX_ENV_MAX + 1];
    pid_t pid;
    int stderr_pipe[2] = {-1, -1};

    if(session->proc.running) {
        errno = EBUSY;
        return -1;
    }

    if(session->tty.backend == PSX_TTY_BACKEND_NONE) {
        errno = ENODEV;
        return -1;
    }

    /*
     * On a console the loader is the preferred way to run a command: the
     * connection it wires in is the only configuration measured to deliver a
     * CLI's library output, arguments reach the wrapper on that same
     * connection, and the wrapper reports the exit status. When no loader
     * answers, the injected spawn below is used, exactly as before.
     */
    if(psx_platform_is_target()) {
        char line[PSX_CMD_LINE_MAX];
        int fd;

        if(session_build_command_line(path, argv, line, sizeof(line)) > 0 &&
           (fd = psx_platform_loader_exec(line)) >= 0) {
            const char *base = strrchr(path, '/');

            session->proc.loader = true;
            session->proc.pid = 0;
            session->proc.relay_out = fd;
            session->proc.relay_in = fd;
            session->proc.exit_code_hint = 0;
            session->proc.exit_reported = false;
            session->proc.running = true;
            session->proc.status = 0;
            session->proc.started_ms = psx_now_ms();

            snprintf(session->command, sizeof(session->command), "%s",
                     base ? base + 1 : path);

            PSX_LOGI("session %u: running \"%s\" through the loader",
                     session->id, line);

            return 0;
        }
    }

    for(size_t i = 0; i < session->env.count; i++) {
        envp[i] = session->env.entries[i];
    }
    envp[session->env.count] = NULL;

    memset(&options, 0, sizeof(options));
    options.path = path;
    options.argv = argv;
    options.envp = envp;

    /*
     * Relay, off: both candidates measured and falsified.
     *
     * Hypothesis one was that the runtime writes its libc stdout to rwpipe,
     * hypothesis two that it writes to the overlapped socket pair. With a
     * daemon-owned pipe imported for rwpipe the session reported "relay error"
     * and the output still did not arrive; with a daemon-owned socket pair
     * imported for rwpair, even the payload's raw descriptor writes stopped
     * appearing, so substituting that pair breaks the runtime's startup as
     * well. The working configuration - the reference's own pair plus a pipe
     * created inside the victim - is what stays until the channel is
     * identified, and the instrumented loader makes that next attempt cheap.
     */
    (void)options.relay_out;
    (void)options.relay_in;
    session->proc.relay_out = -1;
    session->proc.relay_in = -1;

    session->proc.stdin_fd = -1;
    options.stdin_fd = session->tty.slave_fd;
    {
        int stdin_pair[2] = {-1, -1};

        if(psx_socket_pair(stdin_pair) == 0) {
            psx_set_cloexec(stdin_pair[0], true);
            psx_set_cloexec(stdin_pair[1], true);
            psx_set_nonblocking(stdin_pair[0], true);
            options.stdin_fd = stdin_pair[1];
            session->proc.stdin_fd = stdin_pair[0];
        } else {
            PSX_LOGW("session %u: no stdin pair (%s), using the tty",
                     session->id, strerror(errno));
        }
    }
    options.stdout_fd = session->tty.slave_fd;
    options.stderr_fd = session->tty.slave_fd;
    options.cwd = session->cwd;

    /*
     * A real terminal combines both output streams; without one, stderr gets
     * its own channel so the client can tell the two apart. Enabled on the
     * consoles too now: the descriptor install logs which of the three
     * redirections does what, so the case that used to fail silently is
     * observable.
     */
    if(!session->tty.is_real_pty) {
        if(pipe(stderr_pipe) < 0 ||
           psx_set_cloexec(stderr_pipe[0], true) < 0 ||
           psx_set_cloexec(stderr_pipe[1], true) < 0 ||
           psx_set_nonblocking(stderr_pipe[0], true) < 0) {
            int saved_errno = errno;

            if(stderr_pipe[0] >= 0) {
                close(stderr_pipe[0]);
            }
            if(stderr_pipe[1] >= 0) {
                close(stderr_pipe[1]);
            }
            errno = saved_errno;
            return -1;
        }
        options.stderr_fd = stderr_pipe[1];
    }

    if((pid = psx_spawn(&options)) < 0) {
        int saved_errno = errno;

        if(stderr_pipe[0] >= 0) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        errno = saved_errno;
        return -1;
    }

    if(stderr_pipe[1] >= 0) {
        close(stderr_pipe[1]);
    }
    session->proc.stderr_fd = stderr_pipe[0];
    session->proc.pid = pid;
    session->proc.running = true;
    session->proc.status = 0;
    session->proc.started_ms = psx_now_ms();

    {
        const char *base = strrchr(path, '/');

        snprintf(session->command, sizeof(session->command), "%s",
                 base ? base + 1 : path);
    }

    /* Apply the current window size to the new terminal. */
    psx_tty_set_size(&session->tty, session->rows, session->cols);

    PSX_LOGD("session %u: spawned pid %d (%s)", session->id, (int)pid, path);

    return 0;
}

void
psx_session_emit_exit(psx_session_t *session, int exit_code, uint8_t kind)
{
    uint8_t payload[5];
    uint32_t value = (uint32_t)exit_code;

    payload[0] = (uint8_t)(value & 0xff);
    payload[1] = (uint8_t)((value >> 8) & 0xff);
    payload[2] = (uint8_t)((value >> 16) & 0xff);
    payload[3] = (uint8_t)((value >> 24) & 0xff);
    payload[4] = kind;

    psx_session_emit(session, PTTY_MSG_EXIT, payload, sizeof(payload));
}

static void
session_reopen_tty(psx_session_t *session)
{
    psx_tty_backend_t backend = session->tty.backend;
    uint16_t rows = session->tty.rows;
    uint16_t cols = session->tty.cols;

    psx_tty_close(&session->tty);

    if(psx_tty_open(&session->tty, backend, rows, cols) < 0) {
        PSX_LOGW("session %u: cannot recreate %s tty: %s", session->id,
                 psx_tty_backend_name(backend), strerror(errno));
    }

    session->tty_input_closed = false;
}

bool
psx_session_check_process(psx_session_t *session)
{
    int status = 0;
    int rc;
    int exit_code = -1;

    if(!session->proc.running) {
        return false;
    }

    /*
     * A command run through the loader has no pid here: its end of file on the
     * loader connection is what says it is finished, and the status came in on
     * that connection as the wrapper's last line.
     */
    if(session->proc.loader) {
        /*
         * The run is over when the wrapper said so, or when the connection
         * reached its end: waiting only for the end of file left the shell
         * hanging whenever the command kept the socket open, which the user
         * had to interrupt by hand.
         */
        if(session->proc.relay_out >= 0 && !session->proc.exit_reported) {
            return false;
        }

        PSX_LOGI("session %u: \"%s\" finished through the loader (%d)",
                 session->id, session->command,
                 session->proc.exit_code_hint);

        session->proc.running = false;
        session->proc.loader = false;
        session->proc.exit_reported = false;
        session->proc.status = session->proc.exit_code_hint;
        snprintf(session->command, sizeof(session->command), "%s", "psh");

        if(session->proc.relay_in >= 0) {
            close(session->proc.relay_in);
            session->proc.relay_in = -1;
        }

        if(session->proc.relay_out >= 0) {
            close(session->proc.relay_out);
            session->proc.relay_out = -1;
        }

        /* Whatever the command left behind is forwarded before the prompt, so
         * the shell comes back exactly as it does after a locally spawned
         * process. */
        for(int pass = 0; pass < 16; pass++) {
            size_t before = psx_buf_pending(&session->out);

            psx_session_on_tty_readable(session);
            psx_session_on_stderr_readable(session);

            if(psx_buf_pending(&session->out) == before) {
                break;
            }
        }

        if(session->proc.stderr_fd >= 0) {
            close(session->proc.stderr_fd);
            session->proc.stderr_fd = -1;
        }

        if(psx_buf_pending(&session->in) > 0) {
            session->input_discarded_bytes += psx_buf_pending(&session->in);
            psx_buf_free(&session->in);
            psx_buf_init(&session->in);
        }

        if(session->tty_input_closed) {
            session_reopen_tty(session);
        }

        psx_session_emit_exit(session, session->proc.exit_code_hint,
                              PTTY_EXIT_PROCESS);

        if(session->exec_pending) {
            session->exec_pending = false;
            psx_session_emit_exit(session, session->proc.exit_code_hint,
                                  PTTY_EXIT_SHELL);
        }

        /* The prompt is what tells the user the shell is ready again: without
         * it the session looked hung after every command run this way. */
        if(session->shell && session->state == PSX_SESSION_RUNNING) {
            psh_shell_prompt(session->shell);
        }

        return true;
    }

    rc = psx_process_wait(session->proc.pid, &status, 0);

    if(rc == 1) {
        return false;
    }

    if(rc < 0) {
        PSX_LOGW("session %u: waitpid(%d) failed: %s", session->id,
                 (int)session->proc.pid, strerror(errno));
        status = -1;
    }

    session->proc.running = false;
    session->proc.status = status;

    if(session->proc.stdin_fd >= 0) {
        close(session->proc.stdin_fd);
        session->proc.stdin_fd = -1;
    }

    if(session->proc.relay_out >= 0) {
        close(session->proc.relay_out);
        session->proc.relay_out = -1;
    }

    if(session->proc.relay_in >= 0) {
        close(session->proc.relay_in);
        session->proc.relay_in = -1;
    }

    snprintf(session->command, sizeof(session->command), "%s", "psh");

    /*
     * Drain everything the process left in the tty before the session
     * recreates it. A single pass is not enough: the tty read is bounded and
     * whatever stays behind is discarded when the tty is closed, which is
     * exactly how the tail of a process's output used to disappear on
     * hardware.
     */
    for(int pass = 0; pass < 16; pass++) {
        size_t before = psx_buf_pending(&session->out);

        psx_session_on_tty_readable(session);
        psx_session_on_stderr_readable(session);

        if(psx_buf_pending(&session->out) == before) {
            break;
        }
    }

    if(session->proc.stderr_fd >= 0) {
        close(session->proc.stderr_fd);
        session->proc.stderr_fd = -1;
    }

    /*
     * Keystrokes still queued were destined for the process that just exited.
     * Replaying them into the next foreground process would be wrong, so they
     * are discarded - explicitly counted, never silently.
     */
    if(psx_buf_pending(&session->in) > 0) {
        session->input_discarded_bytes += psx_buf_pending(&session->in);
        psx_buf_free(&session->in);
        psx_buf_init(&session->in);
    }

    if(session->tty_input_closed) {
        session_reopen_tty(session);
    }

    if(rc == 0 && WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if(rc == 0 && WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    psx_session_emit_exit(session, exit_code, PTTY_EXIT_PROCESS);

    if(session->exec_pending) {
        session->exec_pending = false;
        psx_session_emit_exit(session, exit_code, PTTY_EXIT_SHELL);
    }

    if(session->shell && session->state == PSX_SESSION_RUNNING) {
        psh_shell_prompt(session->shell);
    }

    PSX_LOGD("session %u: pid %d exited (status %d)", session->id,
             (int)session->proc.pid, status);

    return true;
}

/* --- file transfer ------------------------------------------------------ */

#define PSX_FILE_TEMP_SUFFIX ".psxterm-upload"

static uint64_t
read_le64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void
put_le64(uint8_t *p, uint64_t value)
{
    for(size_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)((value >> (8 * i)) & 0xff);
    }
}

static void
file_transfer_reset(psx_session_t *session, bool remove_temp)
{
    if(session->file_fd >= 0) {
        close(session->file_fd);
    }

    if(remove_temp && session->file_tmp[0]) {
        unlink(session->file_tmp);
    }

    session->file_active = false;
    session->file_fd = -1;
    session->file_mode = 0;
    session->file_offset = 0;
    session->file_size = 0;
    session->file_written = 0;
    session->file_path[0] = '\0';
    session->file_tmp[0] = '\0';
}

static void
file_send_result(psx_session_t *session, uint8_t status, int error,
                 const char *message)
{
    uint8_t payload[5 + 128];
    size_t len = 5;

    payload[0] = status;
    payload[1] = (uint8_t)((uint32_t)error & 0xff);
    payload[2] = (uint8_t)(((uint32_t)error >> 8) & 0xff);
    payload[3] = (uint8_t)(((uint32_t)error >> 16) & 0xff);
    payload[4] = (uint8_t)(((uint32_t)error >> 24) & 0xff);

    if(message && *message) {
        size_t n = strlen(message);

        if(n > sizeof(payload) - 5) {
            n = sizeof(payload) - 5;
        }
        memcpy(payload + 5, message, n);
        len += n;
    }

    psx_session_emit(session, PTTY_MSG_FILE_RESULT, payload, len);
}

int
psx_file_resolve_path(const psx_session_t *session, const uint8_t *raw,
                      size_t len, char *out, size_t out_cap)
{
    char path[PSX_PATH_MAX];

    if(len == 0 || len >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if(memchr(raw, '\0', len)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(path, raw, len);
    path[len] = '\0';

    return psx_session_absolute_path(session, path, out, out_cap);
}

static int
file_handle_open(psx_session_t *session, const uint8_t *payload, size_t len)
{
    uint64_t declared_size;
    char path[PSX_PATH_MAX];
    uint8_t reply[9];
    uint8_t mode;
    struct stat st;

    if(session->file_active) {
        file_send_result(session, 1, EBUSY, "a transfer is already active");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(len < 9) {
        file_send_result(session, 1, EINVAL, "malformed FILE_OPEN");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    mode = payload[0];
    declared_size = read_le64(payload + 1);

    if(psx_file_resolve_path(session, payload + 9, len - 9, path,
                             sizeof(path)) < 0) {
        file_send_result(session, 1, errno, "invalid path");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    session->file_fd = -1;

    if(mode == PTTY_FILE_MODE_READ) {
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            file_send_result(session, 1, errno ? errno : EINVAL,
                             "not a regular file");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        if((session->file_fd = open(path, O_RDONLY)) < 0) {
            file_send_result(session, 1, errno, "cannot open for reading");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        session->file_size = (uint64_t)st.st_size;
    } else if(mode == PTTY_FILE_MODE_WRITE) {
        uint32_t random = 0;
        int written;

        if(psx_platform_random_bytes(&random, sizeof(random)) < 0) {
            random = (uint32_t)psx_now_ms();
        }

        written = snprintf(session->file_tmp, sizeof(session->file_tmp),
                           "%s%s-%d-%08x", path, PSX_FILE_TEMP_SUFFIX,
                           (int)getpid(), (unsigned)random);

        if(written < 0 || (size_t)written >= sizeof(session->file_tmp)) {
            session->file_tmp[0] = '\0';
            file_send_result(session, 1, ENAMETOOLONG,
                             "path too long for a temporary file");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        if((session->file_fd = open(session->file_tmp,
                                    O_CREAT | O_EXCL | O_WRONLY, 0600)) < 0) {
            file_send_result(session, 1, errno, "cannot create the upload");
            session->file_tmp[0] = '\0';
            return PSX_SESSION_FRAME_CONTINUE;
        }

        session->file_size = declared_size;
    } else {
        file_send_result(session, 1, EINVAL, "unknown FILE_OPEN mode");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    snprintf(session->file_path, sizeof(session->file_path), "%s", path);
    session->file_mode = mode;
    session->file_offset = 0;
    session->file_written = 0;
    session->file_active = true;

    reply[0] = 0;
    put_le64(reply + 1, mode == PTTY_FILE_MODE_READ ? session->file_size : 0);
    psx_session_emit(session, PTTY_MSG_FILE_OPEN_OK, reply, sizeof(reply));

    return PSX_SESSION_FRAME_CONTINUE;
}

static int
file_handle_data(psx_session_t *session, const uint8_t *payload, size_t len)
{
    uint64_t offset;
    const uint8_t *data;
    size_t data_len;
    size_t done = 0;

    if(!session->file_active) {
        file_send_result(session, 1, EINVAL, "no transfer is active");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(len < 8) {
        file_send_result(session, 1, EINVAL, "malformed FILE_DATA");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    offset = read_le64(payload);
    data = payload + 8;
    data_len = len - 8;

    if(session->file_mode == PTTY_FILE_MODE_READ) {
        /* Client-driven download: FILE_DATA with no bytes requests a chunk
         * starting at the given offset. */
        if(data_len != 0) {
            file_send_result(session, 1, EINVAL,
                             "download requests carry no data");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        if(offset > session->file_size) {
            file_send_result(session, 1, EINVAL, "offset beyond the file");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        session->file_offset = offset;
        return file_handle_read_request(session);
    }

    if(offset != session->file_offset) {
        file_send_result(session, 1, EINVAL, "unexpected offset");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    while(done < data_len) {
        ssize_t n = pwrite(session->file_fd, data + done, data_len - done,
                           (off_t)(offset + done));

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            file_transfer_reset(session, true);
            file_send_result(session, 1, errno, "write failed");
            return PSX_SESSION_FRAME_CONTINUE;
        }

        done += (size_t)n;
    }

    session->file_offset += data_len;
    session->file_written += data_len;

    return PSX_SESSION_FRAME_CONTINUE;
}

static int
file_handle_seek(psx_session_t *session, const uint8_t *payload, size_t len)
{
    uint64_t offset;

    if(!session->file_active || len < 8) {
        file_send_result(session, 1, EINVAL, "no transfer is active");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    offset = read_le64(payload);

    if(session->file_mode == PTTY_FILE_MODE_READ) {
        if(offset > session->file_size) {
            file_send_result(session, 1, EINVAL, "seek beyond the file");
            return PSX_SESSION_FRAME_CONTINUE;
        }
    } else if(offset > session->file_written) {
        file_send_result(session, 1, EINVAL, "seek beyond written data");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    session->file_offset = offset;
    file_send_result(session, 0, 0, NULL);

    return PSX_SESSION_FRAME_CONTINUE;
}

static int
file_handle_close(psx_session_t *session)
{
    if(!session->file_active) {
        file_send_result(session, 1, EINVAL, "no transfer is active");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(session->file_mode == PTTY_FILE_MODE_READ) {
        file_transfer_reset(session, false);
        file_send_result(session, 0, 0, NULL);
        return PSX_SESSION_FRAME_CONTINUE;
    }

    /* Upload: never leave a partially written final file behind. */
    if(session->file_size != PTTY_FILE_SIZE_UNKNOWN &&
       session->file_written != session->file_size) {
        char message[128];
        int error = EMSGSIZE;

        snprintf(message, sizeof(message),
                 "size mismatch: expected %llu bytes, received %llu",
                 (unsigned long long)session->file_size,
                 (unsigned long long)session->file_written);
        file_transfer_reset(session, true);
        file_send_result(session, 1, error, message);
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(session->file_fd >= 0) {
        if(fsync(session->file_fd) != 0 || close(session->file_fd) != 0) {
            int error = errno;

            session->file_fd = -1;
            file_transfer_reset(session, true);
            file_send_result(session, 1, error, "cannot flush the upload");
            return PSX_SESSION_FRAME_CONTINUE;
        }
        session->file_fd = -1;
    }

    if(rename(session->file_tmp, session->file_path) != 0) {
        int error = errno;

        file_transfer_reset(session, true);
        file_send_result(session, 1, error, "cannot move the upload into place");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    file_transfer_reset(session, false);
    file_send_result(session, 0, 0, NULL);

    return PSX_SESSION_FRAME_CONTINUE;
}

static int
file_handle_stat(psx_session_t *session, const uint8_t *payload, size_t len)
{
    char path[PSX_PATH_MAX];
    struct stat st;
    uint8_t reply[10];

    if(psx_file_resolve_path(session, payload, len, path, sizeof(path)) < 0) {
        file_send_result(session, 1, errno, "invalid path");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(stat(path, &st) != 0) {
        file_send_result(session, 1, errno, "stat failed");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    reply[0] = 0;
    put_le64(reply + 1, (uint64_t)st.st_size);
    reply[9] = S_ISREG(st.st_mode)
                   ? PTTY_FILE_TYPE_REGULAR
                   : (S_ISDIR(st.st_mode) ? PTTY_FILE_TYPE_DIRECTORY
                                          : PTTY_FILE_TYPE_OTHER);

    psx_session_emit(session, PTTY_MSG_FILE_RESULT, reply, sizeof(reply));

    return PSX_SESSION_FRAME_CONTINUE;
}

/*
 * Streams the next chunk of an open read transfer. The client drives the
 * pace: every FILE_DATA it receives is one chunk, so nothing is buffered
 * beyond the current frame.
 */
static int
file_handle_read_request(psx_session_t *session)
{
    uint8_t chunk[4096];
    uint8_t payload[8 + sizeof(chunk)];
    ssize_t n;

    n = pread(session->file_fd, chunk, sizeof(chunk),
              (off_t)session->file_offset);

    if(n < 0) {
        file_transfer_reset(session, false);
        file_send_result(session, 1, errno, "read failed");
        return PSX_SESSION_FRAME_CONTINUE;
    }

    if(n == 0) {
        /* End of file: close implicitly and report success. */
        file_transfer_reset(session, false);
        file_send_result(session, 0, 0, NULL);
        return PSX_SESSION_FRAME_CONTINUE;
    }

    put_le64(payload, session->file_offset);
    memcpy(payload + 8, chunk, (size_t)n);
    session->file_offset += (size_t)n;

    psx_session_emit(session, PTTY_MSG_FILE_DATA, payload, 8 + (size_t)n);

    return PSX_SESSION_FRAME_CONTINUE;
}

/* --- socket IO ---------------------------------------------------------- */

/* Feed the foreground process's stdin pipe, tolerating a full pipe. */
static int
session_write_process_stdin(psx_session_t *session, const uint8_t *data,
                            size_t len)
{
    size_t offset = 0;

    while(offset < len) {
        ssize_t n = write(session->proc.stdin_fd, data + offset, len - offset);

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                /*
                 * The process is not reading. Count what could not be handed
                 * over instead of blocking the whole session loop; the input
                 * queue towards the session already applies backpressure.
                 */
                session->input_discarded_bytes += len - offset;
                return 0;
            }
            PSX_LOGW("session %u: stdin write: %s", session->id,
                     strerror(errno));
            return -1;
        }

        offset += (size_t)n;
    }

    return 0;
}

static int
session_forward_to_process(psx_session_t *session, const uint8_t *payload,
                           size_t len)
{
    size_t offset = 0;

    /*
     * Preferred path: the process reads from its own pipe.
     *
     * End of input is then just closing the write end, which cannot disturb
     * the descriptors the process writes to. On a pipe-backed tty there is no
     * line discipline either, so Ctrl+C is translated into SIGINT here.
     */
    if(session->proc.stdin_fd >= 0) {
        size_t start = 0;

        for(size_t i = 0; i < len; i++) {
            if(payload[i] == 0x03) {
                if(i > start &&
                   session_write_process_stdin(session, payload + start,
                                               i - start) < 0) {
                    return -1;
                }
                psx_process_kill(session->proc.pid, SIGINT);
                start = i + 1;
            }
        }

        if(len == 0) {
            close(session->proc.stdin_fd);
            session->proc.stdin_fd = -1;
            return 0;
        }

        if(len > start &&
           session_write_process_stdin(session, payload + start,
                                       len - start) < 0) {
            return -1;
        }

        return 0;
    }

    /*
     * With a relay, the runtime reads its input from the pipe the daemon owns,
     * so input goes there; it is also written to the terminal because a
     * payload may read its descriptor directly instead of through libc.
     */
    if(session->proc.relay_in >= 0) {
        if(len == 0) {
            /*
             * End of input for a command run through the loader: shut the
             * write direction down rather than closing the socket, because the
             * same connection also carries the command's output and closing it
             * would cut that off too.
             */
            if(session->proc.loader) {
                shutdown(session->proc.relay_in, SHUT_WR);
            } else {
                close(session->proc.relay_in);
                session->proc.relay_in = -1;
            }
        } else {
            ssize_t n = write(session->proc.relay_in, payload, len);

            if(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                PSX_LOGW("session %u: relay write: %s", session->id,
                         strerror(errno));
            }
        }
    }

    /* Zero-length STDIN is an EOF marker for the foreground process. */
    if(len == 0) {
        if(session->tty.is_real_pty) {
            /* Ctrl+D on a canonical terminal delivers EOF. */
            static const uint8_t eof = 0x04;

            return psx_session_write_tty(session, &eof, 1);
        }

        /*
         * PipeTTY: the child shares the slave end of the socketpair, so EOF
         * has to be produced by shutting down the master's write direction.
         * That is not reversible - the session recreates the tty once the
         * process has been reaped.
         */
        if(session->tty.master_fd >= 0) {
            shutdown(session->tty.master_fd, SHUT_WR);
            session->tty_input_closed = true;
        }

        return 0;
    }

    /*
     * PipeTTY has no line discipline: translate Ctrl+C (0x03) into SIGINT
     * for the foreground process. A real PTY does this in the kernel, so the
     * byte is forwarded untouched.
     */
    if(!session->tty.is_real_pty) {
        for(size_t i = 0; i < len; i++) {
            if(payload[i] == 0x03) {
                if(offset < i &&
                   psx_session_write_tty(session, payload + offset,
                                         i - offset) < 0) {
                    return -1;
                }
                offset = i + 1;
                psx_process_kill(session->proc.pid, SIGINT);
            }
        }
    }

    if(offset < len &&
       psx_session_write_tty(session, payload + offset, len - offset) < 0) {
        return -1;
    }

    return 0;
}

/*
 * Client input is consumed by the in-process shell until a line starts an
 * external process; the remainder of the buffer belongs to that process.
 */
static int
session_write_stdin(psx_session_t *session, const uint8_t *payload, size_t len)
{
    size_t offset = 0;

    if(session->proc.running) {
        return session_forward_to_process(session, payload, len);
    }

    /*
     * An explicit zero-length STDIN frame means end-of-input: with no
     * foreground process, the session shell is done (like Ctrl+D on an empty
     * prompt).
     */
    if(len == 0) {
        if(session->shell) {
            psh_shell_request_exit(session->shell, 0);
        }
        return 0;
    }

    while(offset < len) {
        size_t chunk = 0;

        while(offset + chunk < len) {
            uint8_t byte = payload[offset + chunk];

            chunk++;
            if(byte == '\n' || byte == '\r') {
                break;
            }
        }

        if(session->shell) {
            psh_shell_feed(session->shell, payload + offset, chunk);
        }
        offset += chunk;

        if(session->proc.running ||
           (session->shell && psh_shell_exit_requested(session->shell))) {
            break;
        }
    }

    if(offset >= len) {
        return 0;
    }

    if(session->proc.running) {
        return session_forward_to_process(session, payload + offset,
                                          len - offset);
    }

    return 0;
}

int
psx_session_handle_frame(psx_session_t *session, const ptty_header_t *header,
                         const uint8_t *payload)
{
    if(header->session_id != 0 && header->session_id != session->id) {
        PSX_LOGW("session %u: frame for session %u rejected", session->id,
                 header->session_id);
        return -1;
    }

    switch(header->type) {
    case PTTY_MSG_STDIN:
        return session_write_stdin(session, payload, header->payload_length);

    case PTTY_MSG_RESIZE: {
        uint16_t rows = 0;
        uint16_t cols = 0;

        if(ptty_resize_decode(payload, &rows, &cols) < 0) {
            PSX_LOGW("session %u: malformed RESIZE", session->id);
            return -1;
        }

        session->rows = rows;
        session->cols = cols;
        psx_tty_set_size(&session->tty, rows, cols);
        return 0;
    }

    case PTTY_MSG_SIGNAL: {
        int signo = header->payload_length == 1 ? payload[0] : SIGINT;

        if(session->proc.running) {
            psx_process_kill(session->proc.pid, signo);
        } else if(signo == SIGINT && session->shell) {
            static const uint8_t ctrl_c = 0x03;
            psh_shell_feed(session->shell, &ctrl_c, 1);
        }
        return 0;
    }

    case PTTY_MSG_EXEC:
        if(session->proc.running) {
            psx_session_emit(session, PTTY_MSG_STDERR, "psh: busy\r\n", 11);
            return 0;
        }
        if(session->shell && header->payload_length > 0) {
            char line[PSH_LINE_MAX];
            size_t len = header->payload_length;
            int status;

            if(len >= sizeof(line)) {
                len = sizeof(line) - 1;
            }
            memcpy(line, payload, len);
            line[len] = '\0';

            session->exec_pending = true;
            status = psh_shell_execute_line(session->shell, line);

            if(session->proc.running) {
                /* EXIT frames are emitted when the process finishes. */
                return 0;
            }

            session->exec_pending = false;
            psx_session_emit_exit(session, status, PTTY_EXIT_SHELL);

            if(!psh_shell_exit_requested(session->shell)) {
                psh_shell_prompt(session->shell);
            }
        }
        return 0;

    case PTTY_MSG_DIAG_REQUEST: {
        uint8_t flags = header->payload_length >= 1 ? payload[0] : 0;
        psx_diag_report_t report;
        psx_diag_status_t overall;
        char *text;
        size_t needed;
        int exit_code;

        if(session->proc.running) {
            static const char busy[] =
                "[PSXTerm: a foreground process is running; diagnostics need "
                "an idle session]\n";
            uint8_t code = PSX_DIAG_EXIT_FAILED;

            psx_session_emit(session, PTTY_MSG_DIAG_DATA, busy,
                             sizeof(busy) - 1);
            psx_session_emit(session, PTTY_MSG_DIAG_DONE, &code, 1);
            return 0;
        }

        psx_diag_report_init(&report);
        psx_diag_run(&report, session);

        needed = (flags & PTTY_DIAG_FLAG_JSON)
                     ? psx_diag_format_json(&report, NULL, 0)
                     : psx_diag_format_human(&report, NULL, 0);

        if((text = malloc(needed + 1))) {
            if(flags & PTTY_DIAG_FLAG_JSON) {
                psx_diag_format_json(&report, text, needed + 1);
            } else {
                psx_diag_format_human(&report, text, needed + 1);
            }

            psx_session_emit(session, PTTY_MSG_DIAG_DATA, text, needed);
            free(text);
        }

        overall = psx_diag_overall_status(&report);
        exit_code = psx_diag_exit_code(overall);
        psx_diag_report_free(&report);

        psx_session_emit(session, PTTY_MSG_DIAG_DONE,
                         (uint8_t *)&exit_code, 1);
        return 0;
    }

    case PTTY_MSG_SESSIONS_REQUEST: {
        static const char header_line[] =
            "  ID  STATE       CLIENT           COMMAND\n";
        const psx_session_manager_t *manager = session->manager;

        psx_session_emit(session, PTTY_MSG_SESSIONS_DATA, header_line,
                         sizeof(header_line) - 1);

        if(manager) {
            for(const psx_session_t *s = manager->sessions; s; s = s->next) {
                char line[256];
                int len = snprintf(line, sizeof(line),
                                   "%4u  %-10s  %-15.15s  %.31s\n", s->id,
                                   psx_session_state_name(s->state),
                                   s->client_name[0] ? s->client_name : "-",
                                   s->command);

                if(len > 0) {
                    psx_session_emit(session, PTTY_MSG_SESSIONS_DATA, line,
                                     (size_t)len);
                }
            }
        }

        return psx_session_emit(session, PTTY_MSG_SESSIONS_DONE, NULL, 0) < 0
                   ? PSX_SESSION_FRAME_ERROR
                   : PSX_SESSION_FRAME_CONTINUE;
    }

    case PTTY_MSG_FILE_OPEN:
        return file_handle_open(session, payload, header->payload_length);

    case PTTY_MSG_FILE_DATA:
        return file_handle_data(session, payload, header->payload_length);

    case PTTY_MSG_FILE_SEEK:
        return file_handle_seek(session, payload, header->payload_length);

    case PTTY_MSG_FILE_CLOSE:
        return file_handle_close(session);

    case PTTY_MSG_FILE_STAT:
        return file_handle_stat(session, payload, header->payload_length);

    case PTTY_MSG_DETACH:
        return PSX_SESSION_FRAME_DETACH;

    case PTTY_MSG_SHUTDOWN:
        /* Administrative command during bring-up: report it to the server,
         * which owns the daemon loop, and close this connection cleanly. */
        session->shutdown_requested = true;
        psx_session_emit(session, PTTY_MSG_CLOSE, NULL, 0);
        session->state = PSX_SESSION_CLOSING;
        return PSX_SESSION_FRAME_CLOSE;

    case PTTY_MSG_PING:
        return psx_session_emit(session, PTTY_MSG_PONG, payload,
                                header->payload_length) < 0
                   ? -1
                   : 0;

    case PTTY_MSG_PONG:
        return 0;

    case PTTY_MSG_CLOSE:
        session->state = PSX_SESSION_CLOSING;
        return PSX_SESSION_FRAME_CLOSE;

    default:
        PSX_LOGW("session %u: unexpected %s frame", session->id,
                 ptty_msg_name(header->type));
        return PSX_SESSION_FRAME_ERROR;
    }
}

int
psx_session_on_socket_readable(psx_session_t *session)
{
    for(;;) {
        ptty_header_t header;
        const uint8_t *payload = NULL;
        ptty_read_result_t rc;
        int handled;

        /*
         * Backpressure: while the foreground process is not consuming input,
         * stop reading from the socket. The bytes stay in the kernel socket
         * buffer, TCP eventually reports a zero window, and nothing is
         * dropped. Reads resume once the tty drains below the watermark.
         */
        if(psx_buf_pending(&session->in) >= PSX_SESSION_IN_HIGH_WATER) {
            session->input_backpressure_events++;
            return 0;
        }

        rc = ptty_read_frame(&session->reader, &header, &payload);

        if(rc == PTTY_READ_AGAIN) {
            return 0;
        }
        if(rc == PTTY_READ_EOF) {
            PSX_LOGD("session %u: client disconnected", session->id);
            return -1;
        }
        if(rc == PTTY_READ_ERROR) {
            PSX_LOGW("session %u: socket error: %s", session->id,
                     strerror(errno));
            return -1;
        }
        if(rc == PTTY_READ_PROTOCOL) {
            PSX_LOGW("session %u: protocol error, closing", session->id);
            return -1;
        }

        psx_session_touch(session);
        handled = psx_session_handle_frame(session, &header, payload);

        if(handled != 0) {
            return handled;
        }
    }
}

/* --- tty IO ------------------------------------------------------------- */

int
psx_session_write_tty(psx_session_t *session, const uint8_t *data, size_t len)
{
    if(session->tty.master_fd < 0) {
        errno = ENODEV;
        return -1;
    }

    if(psx_buf_pending(&session->in) >= PSX_SESSION_IN_HIGH_WATER) {
        session->input_backpressure_events++;
    }

    /*
     * Callers only reach this point below the high watermark, which already
     * reserves room for one maximum frame; enforce the hard bound anyway so
     * input is never silently dropped or buffered without limit.
     */
    if(psx_buf_pending(&session->in) + len > PSX_SESSION_IN_MAX) {
        errno = ENOBUFS;
        return -1;
    }

    if(psx_buf_append(&session->in, data, len) < 0) {
        return -1;
    }

    return psx_session_flush_tty_input(session);
}

int
psx_session_flush_tty_input(psx_session_t *session)
{
    if(psx_buf_pending(&session->in) == 0) {
        return 0;
    }

    if(psx_buf_flush(&session->in, session->tty.master_fd) < 0) {
        return -1;
    }

    return 0;
}

size_t
psx_session_pending_tty_input(const psx_session_t *session)
{
    return psx_buf_pending(&session->in);
}

int
psx_session_on_tty_readable(psx_session_t *session)
{
    uint8_t buffer[4096];

    if(session->tty.master_fd < 0) {
        return 0;
    }

    for(;;) {
        ssize_t n = read(session->tty.master_fd, buffer, sizeof(buffer));

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            /* A real PTY master returns EIO once the last slave is gone. */
            if(errno == EIO) {
                return 0;
            }
            PSX_LOGW("session %u: tty read: %s", session->id, strerror(errno));
            return -1;
        }

        if(n == 0) {
            return 0;
        }

        PSX_LOGD("session %u: tty read %zd bytes", session->id, n);

        if(psx_session_emit(session, PTTY_MSG_STDOUT, buffer, (size_t)n) < 0) {
            return -1;
        }
    }
}

/*
 * The wrapper payload ends its output with "PSXTERM-EXIT <status>". Keep that
 * line out of the client's stream and remember the status instead, which is
 * the only report of it the loader path can give.
 */
static int
session_scan_exit_marker(psx_session_t *session, const uint8_t *data,
                         size_t len)
{
    static const char marker[] = "PSXTERM-EXIT ";
    const size_t mlen = sizeof(marker) - 1;
    size_t head = len;

    for(size_t i = 0; i + mlen < len; i++) {
        if(memcmp(data + i, marker, mlen) != 0) {
            continue;
        }

        {
            char digits[16];
            size_t k = 0;

            for(size_t j = i + mlen; j < len && k + 1 < sizeof(digits); j++) {
                if(data[j] < '0' || data[j] > '9') {
                    break;
                }
                digits[k++] = (char)data[j];
            }
            digits[k] = '\0';

            if(k > 0) {
                session->proc.exit_code_hint = atoi(digits);
                session->proc.exit_reported = true;
                head = i;
            }
        }
        break;
    }

    if(head > 0 &&
       psx_session_emit(session, PTTY_MSG_STDOUT, data, head) < 0) {
        return -1;
    }

    return 0;
}

/*
 * The payload runtime's own output.
 *
 * A console payload writes its libc stdout through the handles the loader
 * gives it rather than through fd 1, so this relay is where a normal CLI's
 * printf output actually arrives. It is presented to the client as ordinary
 * STDOUT, next to whatever the payload wrote to its descriptor directly.
 */
int
psx_session_on_relay_readable(psx_session_t *session)
{
    uint8_t buffer[4096];
    int *fds[2] = {&session->proc.relay_out, &session->proc.relay_in};

    /*
     * Both ends are drained: the pair belongs to the daemon, and which side
     * the payload runtime writes to is exactly what this is establishing. The
     * bytes go out as ordinary STDOUT, next to whatever the payload wrote to
     * its descriptors directly.
     */
    for(size_t i = 0; i < 2; i++) {
        while(*fds[i] >= 0) {
            ssize_t n = read(*fds[i], buffer, sizeof(buffer));

            if(n < 0) {
                if(errno == EINTR) {
                    continue;
                }
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                PSX_LOGW("session %u: relay read: %s", session->id,
                         strerror(errno));
                return -1;
            }
            if(n == 0) {
                close(*fds[i]);
                if(session->proc.relay_out == session->proc.relay_in) {
                    session->proc.relay_in = -1;
                }
                *fds[i] = -1;
                break;
            }

            if(session->proc.loader) {
                if(session_scan_exit_marker(session, buffer, (size_t)n) < 0) {
                    return -1;
                }
                continue;
            }

            if(psx_session_emit(session, PTTY_MSG_STDOUT, buffer,
                                (size_t)n) < 0) {
                return -1;
            }
        }
    }

    return 0;
}

int
psx_session_on_stderr_readable(psx_session_t *session)
{
    uint8_t buffer[4096];

    if(session->proc.stderr_fd < 0) {
        return 0;
    }

    for(;;) {
        ssize_t n = read(session->proc.stderr_fd, buffer, sizeof(buffer));

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            PSX_LOGW("session %u: stderr read: %s", session->id, strerror(errno));
            return -1;
        }
        if(n == 0) {
            close(session->proc.stderr_fd);
            session->proc.stderr_fd = -1;
            return 0;
        }
        if(psx_session_emit(session, PTTY_MSG_STDERR, buffer, (size_t)n) < 0) {
            return -1;
        }
    }
}

/* --- session manager ---------------------------------------------------- */

void
psx_session_manager_init(psx_session_manager_t *manager, size_t max)
{
    memset(manager, 0, sizeof(*manager));
    manager->max = max ? max : 1;
    manager->next_id = 1;
}

psx_session_t *
psx_session_manager_create(psx_session_manager_t *manager, int sock_fd)
{
    psx_session_t *session;
    uint32_t id;

    if(manager->count >= manager->max) {
        errno = ENOSPC;
        return NULL;
    }

    id = manager->next_id++;
    if(id == 0) {
        id = manager->next_id++;
    }

    if(!(session = psx_session_create(id, sock_fd))) {
        return NULL;
    }

    session->manager = manager;
    session->next = manager->sessions;
    manager->sessions = session;
    manager->count++;

    return session;
}

void
psx_session_manager_remove(psx_session_manager_t *manager,
                           psx_session_t *session)
{
    psx_session_t **link = &manager->sessions;

    while(*link && *link != session) {
        link = &(*link)->next;
    }

    if(!*link) {
        return;
    }

    *link = session->next;
    manager->count--;
    session->next = NULL;

    psx_session_destroy(session);
}

psx_session_t *
psx_session_manager_find(psx_session_manager_t *manager, uint32_t id)
{
    for(psx_session_t *s = manager->sessions; s; s = s->next) {
        if(s->id == id) {
            return s;
        }
    }

    return NULL;
}

size_t
psx_session_manager_count(const psx_session_manager_t *manager)
{
    return manager->count;
}
