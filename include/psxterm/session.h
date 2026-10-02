#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "psxterm/process.h"
#include "psxterm/protocol.h"
#include "psxterm/tty.h"
#include "psxterm/util.h"

#define PSX_PATH_MAX 512
#define PSX_ENV_MAX 64

/*
 * Backpressure bounds.
 *
 * PSX_SESSION_OUT_MAX bounds the queue towards a client that stops reading.
 * PSX_SESSION_IN_MAX bounds the queue towards a foreground process that does
 * not consume input; above PSX_SESSION_IN_HIGH_WATER the session stops
 * reading from its socket so TCP backpressure reaches the client instead of
 * growing the queue. The gap is larger than one maximum frame so a single
 * 64 KiB STDIN frame can never push the queue past the hard limit.
 */
#define PSX_SESSION_OUT_MAX (8u * 1024u * 1024u)
#define PSX_SESSION_IN_MAX (512u * 1024u)
#define PSX_SESSION_IN_HIGH_WATER (PSX_SESSION_IN_MAX - PTTY_MAX_PAYLOAD)

/*
 * Bounded ring buffer that holds terminal output while a session is detached.
 * When it fills, the oldest bytes are dropped and the loss is reported to the
 * client on reattach; a detached session never grows without limit.
 */
#define PSX_SESSION_SCROLLBACK_MAX (256u * 1024u)

/* --- session environment ------------------------------------------------ */

typedef struct {
    char *entries[PSX_ENV_MAX];
    size_t count;
} psx_env_t;

/* Fill with platform defaults plus an optional NULL-terminated inherit list. */
void psx_env_init(psx_env_t *env, char *const *inherit);
void psx_env_clear(psx_env_t *env);

const char *psx_env_get(const psx_env_t *env, const char *name);
int psx_env_set(psx_env_t *env, const char *name, const char *value);
int psx_env_set_entry(psx_env_t *env, const char *name_eq_value);
int psx_env_unset(psx_env_t *env, const char *name);
size_t psx_env_count(const psx_env_t *env);
const char *psx_env_entry(const psx_env_t *env, size_t index);

/* --- sessions ----------------------------------------------------------- */

typedef enum {
    PSX_SESSION_ACCEPTED = 0,
    PSX_SESSION_HANDSHAKING,
    PSX_SESSION_RUNNING,
    PSX_SESSION_DETACHED,
    PSX_SESSION_CLOSING,
    PSX_SESSION_CLOSED
} psx_session_state_t;

struct psx_shell;
struct psx_session_manager;

typedef struct psx_session {
    uint32_t id;
    psx_session_state_t state;
    int sock_fd;

    char client_name[64];
    char term[64];

    psx_tty_t tty;
    psx_env_t env;
    char cwd[PSX_PATH_MAX];
    uint16_t rows;
    uint16_t cols;

    uint64_t created_ms;
    uint64_t last_activity_ms;
    uint64_t handshake_deadline_ms;
    uint64_t close_deadline_ms;

    bool exec_pending;

    /* Diagnostic counters (frames exchanged on this session's socket). */
    uint64_t frames_in;
    uint64_t frames_out;
    uint32_t protocol_errors;

    /* Capability bitmask advertised to the client (PTTY_CAP_*). */
    uint32_t capabilities;

    /* Backpressure accounting. */
    uint64_t input_backpressure_events;
    uint64_t input_discarded_bytes;

    /*
     * Session persistence. The resume token is a secret: it authenticates
     * reattachment and is never written to the daemon log.
     */
    uint8_t resume_token[PTTY_RESUME_TOKEN_SIZE];
    uint64_t detached_since_ms;
    uint8_t *scrollback;
    size_t scrollback_cap;
    size_t scrollback_len;
    size_t scrollback_start;
    bool scrollback_truncated;

    /* Short description of the current foreground command ("psh" when idle). */
    char command[64];

    /* At most one file transfer at a time, per session. */
    bool file_active;
    int file_fd;
    uint8_t file_mode;
    uint64_t file_offset;  /* next read/write offset */
    uint64_t file_size;    /* declared size for writes, actual for reads */
    uint64_t file_written; /* bytes accepted so far for writes */
    char file_path[PSX_PATH_MAX];
    char file_tmp[PSX_PATH_MAX];

    /* PipeTTY only: input was shut down to deliver EOF, so the tty must be
     * recreated before the next process is spawned. */
    bool tty_input_closed;

    /* Set when the client asked the daemon to shut down (bring-up helper). */
    bool shutdown_requested;

    /* 0 while the shell runs in-process (this build); reserved for a
     * process-backed shell on target platforms. */
    pid_t shell_pid;

    psx_process_t proc;

    psx_buf_t out;
    psx_buf_t in;
    ptty_reader_t reader;

    struct psx_shell *shell;

    const struct psx_session_manager *manager;

    struct psx_session *next;
} psx_session_t;

psx_session_t *psx_session_create(uint32_t id, int sock_fd);
void psx_session_destroy(psx_session_t *session);

/* Create tty, environment, cwd and the in-process shell; emit the banner. */
int psx_session_begin(psx_session_t *session);

/*
 * Session persistence.
 *
 * Detaching keeps cwd, environment, shell, tty, foreground process and
 * dimensions, and buffers further output in a bounded scrollback. Attaching
 * takes over a new socket (validated with the resume token), reports whether
 * output was lost while detached and resumes normal operation.
 */
void psx_session_detach(psx_session_t *session);
int psx_session_attach(psx_session_t *session, int sock_fd);
bool psx_session_check_token(const psx_session_t *session,
                             const uint8_t *token, size_t len);
bool psx_session_is_detached(const psx_session_t *session);

/*
 * Append to the bounded detached scrollback. Exposed for tests; sessions use
 * it internally when emitting while detached.
 */
void psx_session_scrollback_append(psx_session_t *session, const uint8_t *data,
                                   size_t len);

/*
 * Validate a transfer path received over the network and resolve it against
 * the session cwd. Rejects empty paths, embedded NUL bytes and paths that do
 * not fit. Returns 0 or -1 with errno set.
 */
int psx_file_resolve_path(const psx_session_t *session, const uint8_t *raw,
                          size_t len, char *out, size_t out_cap);

/* Queue a framed message for the client. */
int psx_session_emit(psx_session_t *session, uint8_t type, const void *data,
                     size_t len);
int psx_session_emit_fmt(psx_session_t *session, uint8_t type, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Flush pending output. Returns 0 on success, -1 on error. */
int psx_session_flush(psx_session_t *session);
void psx_session_touch(psx_session_t *session);

/* Socket side: read frames and dispatch. Returns 0 on success, -1 on error. */
int psx_session_on_socket_readable(psx_session_t *session);

/* TTY side: forward foreground process output to the client. */
int psx_session_on_tty_readable(psx_session_t *session);

/* Reap a finished foreground process and emit EXIT. Returns true if reaped. */
bool psx_session_check_process(psx_session_t *session);

/* Resolve `path` on PATH, spawn it on the session tty, switch to process
 * mode. Returns 0 on success, -1 on error. */
int psx_session_spawn_process(psx_session_t *session, const char *path,
                              char *const *argv);

/* Resolve a command name against PATH. Returns a malloc'd absolute path or
 * NULL. Caller frees. */
char *psx_session_resolve_path(psx_session_t *session, const char *name);

/*
 * Turn `path` into an absolute, normalized path using the session cwd and
 * HOME. The target does not need to exist. Returns 0 or -1 (errno set).
 */
int psx_session_absolute_path(const psx_session_t *session, const char *path,
                              char *out, size_t out_cap);

bool psx_session_has_process(const psx_session_t *session);

/* Runtime capability bitmask (PTTY_CAP_*) advertised with PTTY_MSG_CAPS. */
uint32_t psx_session_capabilities(const psx_session_t *session);

/* Queue an EXIT frame (kind: PTTY_EXIT_PROCESS or PTTY_EXIT_SHELL). */
void psx_session_emit_exit(psx_session_t *session, int exit_code, uint8_t kind);

/* Raw keystrokes for the foreground process; queued if the tty is not
 * writable right now. */
int psx_session_write_tty(psx_session_t *session, const uint8_t *data, size_t len);
int psx_session_flush_tty_input(psx_session_t *session);
size_t psx_session_pending_tty_input(const psx_session_t *session);

const char *psx_session_state_name(psx_session_state_t state);

/* Handle one frame for a running session. Returns 0 to continue, -1 on fatal
 * protocol/IO error (server closes the session). */
int psx_session_handle_frame(psx_session_t *session, const ptty_header_t *header,
                             const uint8_t *payload);

/* Outcome of a handled frame, shared by session and handshake handlers. */
enum {
    PSX_SESSION_FRAME_CONTINUE = 0,
    PSX_SESSION_FRAME_CLOSE = 1,    /* destroy the session */
    PSX_SESSION_FRAME_DETACH = 2,   /* keep it alive without a client */
    PSX_SESSION_FRAME_REPLACED = 3, /* connection handed to another session */
    PSX_SESSION_FRAME_ERROR = -1
};

/* --- session manager ---------------------------------------------------- */

typedef struct psx_session_manager {
    psx_session_t *sessions;
    size_t count;
    size_t max;
    uint32_t next_id;
} psx_session_manager_t;

void psx_session_manager_init(psx_session_manager_t *manager, size_t max);
psx_session_t *psx_session_manager_create(psx_session_manager_t *manager,
                                          int sock_fd);
void psx_session_manager_remove(psx_session_manager_t *manager,
                                psx_session_t *session);
psx_session_t *psx_session_manager_find(psx_session_manager_t *manager,
                                        uint32_t id);
size_t psx_session_manager_count(const psx_session_manager_t *manager);
