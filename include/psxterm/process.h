#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Hard bounds used when a spawn request is copied into a guaranteed
 * NUL-terminated argument and environment vector: a caller that forgets the
 * terminator would otherwise make the kernel read past the array.
 */
#define PSX_SPAWN_MAX_ARGS 64
#define PSX_SPAWN_MAX_ENV 128

/*
 * External process execution.
 *
 * The shell only ever calls psx_spawn(). Platform internals (fork/exec on the
 * host, elfldr-style payload injection on PS4/PS5) stay under platform/.
 */

/* Session-owned view of a spawned process. */
typedef struct {
    pid_t pid;
    bool running;
    int status; /* wait status once exited */
    uint64_t started_ms;

    /*
     * Daemon's end of the foreground process's stdin pipe. Kept separate from
     * the terminal pair so the end of input can be signalled without touching
     * the descriptors the process writes its output to.
     */
    int stdin_fd;

    /* Session-owned read end of a separately captured stderr stream, or -1
     * when the backend writes both output streams to the terminal. */
    int stderr_fd;

    /*
     * Relay for the payload runtime's own stdio handles: relay_out is the
     * daemon's read end (the payload's libc stdout arrives there) and relay_in
     * its write end (input for the payload's runtime). Both -1 when the
     * backend is not relaying.
     */
    int relay_out;
    int relay_in;
} psx_process_t;

typedef struct {
    const char *path;

    char *const *argv;
    char *const *envp;

    int stdin_fd;
    int stdout_fd;
    int stderr_fd;

    const char *cwd;

    /*
     * Optional relay for the payload runtime's own stdio handles.
     *
     * A console payload writes its libc stdout through the handles the loader
     * passes it, not through fd 1, so a loader that wants to see it has to own
     * the other end of that channel. When both pointers are given, the backend
     * builds the channel out of a real pipe it owns, hands the payload its
     * ends, and writes the daemon's ends here (-1 when no relay was set up).
     * Both pointers must be NULL when no relay is wanted.
     */
    int *relay_out;
    int *relay_in;
} psx_spawn_options_t;

/*
 * Spawn a program. Returns the new pid on success, -1 on error (errno set).
 * A pid of 0 is never returned. Not supported backends fail with ENOSYS.
 */
pid_t psx_spawn(const psx_spawn_options_t *options);

/*
 * Stages a spawn attempt passes through. Backends advance the stage as they
 * work so a failure can be attributed to a specific step instead of a single
 * "spawn failed" message. Each backend uses the stages it actually performs;
 * for example the host backend never attaches to a victim process.
 */
typedef enum {
    PSX_SPAWN_STAGE_PREPARE = 0,
    PSX_SPAWN_STAGE_CREATE_VICTIM,
    PSX_SPAWN_STAGE_ATTACH,
    PSX_SPAWN_STAGE_RAISE_PRIVILEGES,
    PSX_SPAWN_STAGE_DUP_STDIO,
    PSX_SPAWN_STAGE_LOAD_ELF,
    PSX_SPAWN_STAGE_RELOCATE,
    PSX_SPAWN_STAGE_SET_REGISTERS,
    PSX_SPAWN_STAGE_DETACH,
    PSX_SPAWN_STAGE_RUNNING
} psx_spawn_stage_t;

typedef struct {
    psx_spawn_stage_t stage;
    int error_code; /* errno-style code, 0 when unknown */
    char detail[128]; /* short, non-sensitive description */
} psx_spawn_failure_t;

const char *psx_spawn_stage_name(psx_spawn_stage_t stage);

/*
 * Like psx_spawn(), but reports the stage, error code and a short detail for
 * a failed attempt. `failure` may be NULL. On success the failure stage is
 * PSX_SPAWN_STAGE_RUNNING.
 */
pid_t psx_spawn_ex(const psx_spawn_options_t *options,
                   psx_spawn_failure_t *failure);

/*
 * Reap a child.
 *   timeout_ms < 0  : block until the child exits
 *   timeout_ms == 0 : poll
 * Returns 0 when the child exited (*status_out filled with wait status),
 * 1 when it is still running, -1 on error.
 */
int psx_process_wait(pid_t pid, int *status_out, int timeout_ms);

int psx_process_kill(pid_t pid, int sig);

bool psx_process_backend_available(void);
const char *psx_process_backend_name(void);

/* --- platform hooks, implemented under platform/<plat>/process.c -------- */
pid_t psx_platform_spawn(const psx_spawn_options_t *options,
                         psx_spawn_failure_t *failure);
int psx_platform_process_wait(pid_t pid, int *status_out, int timeout_ms);
int psx_platform_process_kill(pid_t pid, int sig);
bool psx_platform_process_available(void);
const char *psx_platform_process_name(void);
