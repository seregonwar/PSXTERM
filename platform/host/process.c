#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

pid_t
psx_platform_spawn(const psx_spawn_options_t *options)
{
    pid_t pid = fork();

    if(pid < 0) {
        return -1;
    }

    if(pid == 0) {
        /* Child */
        int tty_fd = options->stdin_fd;

        signal(SIGPIPE, SIG_DFL);

        if(tty_fd >= 0) {
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
