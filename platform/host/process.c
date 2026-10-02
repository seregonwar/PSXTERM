#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psxterm/process.h"
#include "psxterm/util.h"

/*
 * Host backend: ordinary fork(2) + dup2(2) + execve(2).
 *
 * The child becomes a session leader of its own session and, when a real PTY
 * is attached, takes it as the controlling terminal so that Ctrl+C reaches
 * the foreground process group through the line discipline.
 */

static void
stage_set(psx_spawn_failure_t *failure, psx_spawn_stage_t stage, int error_code,
          const char *detail)
{
    if(!failure) {
        return;
    }

    failure->stage = stage;
    failure->error_code = error_code;
    failure->detail[0] = '\0';

    if(detail) {
        snprintf(failure->detail, sizeof(failure->detail), "%s", detail);
    }
}

pid_t
psx_platform_spawn(const psx_spawn_options_t *options,
                   psx_spawn_failure_t *failure)
{
    pid_t pid;

    stage_set(failure, PSX_SPAWN_STAGE_PREPARE, 0, NULL);

    if(!options->path || !*options->path) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, EINVAL,
                  "empty executable path");
        errno = EINVAL;
        return -1;
    }

    stage_set(failure, PSX_SPAWN_STAGE_CREATE_VICTIM, 0, NULL);

    if((pid = fork()) < 0) {
        stage_set(failure, PSX_SPAWN_STAGE_CREATE_VICTIM, errno,
                  "fork failed");
        return -1;
    }

    if(pid == 0) {
        /* Child: failures are reported through the exit status (126 setup,
         * 127 not found, 126 other exec errors). */
        int tty_fd = options->stdin_fd;

        signal(SIGPIPE, SIG_DFL);

        if(tty_fd >= 0) {
            struct sigaction sa;

            /*
             * The child must not inherit the daemon's signal handlers, in
             * particular not the SIGINT/SIGTERM handler that only writes to
             * the shutdown pipe.
             */
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGTERM, &sa, NULL);
            sigaction(SIGQUIT, &sa, NULL);
            sigaction(SIGHUP, &sa, NULL);

            setsid();
            ioctl(tty_fd, TIOCSCTTY, 0);

            if(dup2(options->stdin_fd, STDIN_FILENO) < 0 ||
               dup2(options->stdout_fd, STDOUT_FILENO) < 0 ||
               dup2(options->stderr_fd, STDERR_FILENO) < 0) {
                _exit(126);
            }

            if(options->stdin_fd > STDERR_FILENO) {
                close(options->stdin_fd);
            }
            if(options->stdout_fd > STDERR_FILENO &&
               options->stdout_fd != options->stdin_fd) {
                close(options->stdout_fd);
            }
            if(options->stderr_fd > STDERR_FILENO &&
               options->stderr_fd != options->stdin_fd &&
               options->stderr_fd != options->stdout_fd) {
                close(options->stderr_fd);
            }
        }

        if(options->cwd && chdir(options->cwd) < 0) {
            _exit(126);
        }

        execve(options->path, options->argv, options->envp);

        _exit(errno == ENOENT ? 127 : 126);
    }

    stage_set(failure, PSX_SPAWN_STAGE_RUNNING, 0, NULL);

    return pid;
}

int
psx_platform_process_wait(pid_t pid, int *status_out, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? psx_now_ms() + (uint64_t)timeout_ms : 0;

    for(;;) {
        pid_t rc = waitpid(pid, status_out, timeout_ms == 0 ? WNOHANG : 0);

        if(rc == pid) {
            return 0;
        }

        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }

        if(timeout_ms == 0 || (timeout_ms > 0 && psx_now_ms() >= deadline)) {
            return 1;
        }
    }
}

int
psx_platform_process_kill(pid_t pid, int sig)
{
    return kill(pid, sig);
}

bool
psx_platform_process_available(void)
{
    return true;
}

const char *
psx_platform_process_name(void)
{
    return "fork-exec";
}
