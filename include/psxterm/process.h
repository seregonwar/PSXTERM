#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

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
} psx_process_t;

typedef struct {
    const char *path;

    char *const *argv;
    char *const *envp;

    int stdin_fd;
    int stdout_fd;
    int stderr_fd;

    const char *cwd;
} psx_spawn_options_t;

/*
 * Spawn a program. Returns the new pid on success, -1 on error (errno set).
 * A pid of 0 is never returned. Not supported backends fail with ENOSYS.
 */
pid_t psx_spawn(const psx_spawn_options_t *options);

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
pid_t psx_platform_spawn(const psx_spawn_options_t *options);
int psx_platform_process_wait(pid_t pid, int *status_out, int timeout_ms);
int psx_platform_process_kill(pid_t pid, int sig);
bool psx_platform_process_available(void);
const char *psx_platform_process_name(void);
