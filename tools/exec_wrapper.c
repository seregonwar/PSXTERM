/*
 * psxterm-exec: run one command on behalf of the daemon.
 *
 * A console payload launched by a loader (elfldr on port 9021) receives the
 * loader's connection as its standard io, and that socket is what makes a
 * normal CLI's output visible - measured on hardware, both descriptor writes
 * and libc printf reach it. This program is therefore the piece that runs a
 * real CLI: the daemon sends it through the loader, then writes the command
 * line here, and this process spawns the command with the same standard io it
 * was given and reports the exit status the loader does not.
 *
 * Protocol, one line of text per run:
 *
 *     <path to ELF> [args...]
 *
 * Everything after the first space is split on spaces, so the daemon has to
 * quote arguments that contain spaces the way it would for a shell. The reply
 * is the command's own output, then a final line:
 *
 *     PSXTERM-EXIT <status>
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/process.h"
#include "psxterm/runtime.h"

#define EXEC_LINE_MAX 1024
#define EXEC_ARGS_MAX 64

static int
read_line(char *out, size_t cap)
{
    size_t used = 0;

    while(used + 1 < cap) {
        char ch = '\0';
        ssize_t got = read(STDIN_FILENO, &ch, 1);

        if(got < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(got == 0) {
            break;
        }
        if(ch == '\n') {
            break;
        }

        out[used++] = ch;
    }

    out[used] = '\0';

    return (int)used;
}

int
main(void)
{
    char line[EXEC_LINE_MAX];
    char *argv[EXEC_ARGS_MAX + 1];
    size_t argc = 0;
    psx_spawn_options_t options;
    psx_spawn_failure_t failure;
    int status = -1;
    pid_t pid;

    /*
     * This process's standard output belongs to the user: the log goes to the
     * platform sink only, so the internals of running the command never mix
     * with what the command itself prints.
     */
    psx_log_set_stream(false);

    if(!psx_platform_init()) {
        printf("PSXTERM-EXIT %d\n", -1);
        return 1;
    }

    if(read_line(line, sizeof(line)) <= 0) {
        printf("PSXTERM-EXIT %d\n", -1);
        return 1;
    }

    for(char *token = strtok(line, " "); token; token = strtok(NULL, " ")) {
        if(argc >= EXEC_ARGS_MAX) {
            break;
        }
        argv[argc++] = token;
    }
    argv[argc] = NULL;

    if(argc == 0) {
        printf("PSXTERM-EXIT %d\n", -1);
        return 1;
    }

    memset(&options, 0, sizeof(options));
    options.path = argv[0];
    options.argv = argv;

    /*
     * Under the loader this process inherits an almost empty environment, so
     * the command gets the runtime layout's instead: home, tmp, PATH and the
     * certificate bundle, which is what a CLI like curl needs.
     */
    {
        static char *envp[24];
        size_t count = psx_runtime_environment(envp, 22);

        if(count == 0) {
            options.envp = psx_platform_inherit_environ();
        } else {
            envp[count] = NULL;
            options.envp = envp;
        }
    }
    /* The loader's socket, handed down unchanged - including to the child. */
    options.stdin_fd = STDIN_FILENO;
    options.stdout_fd = STDOUT_FILENO;
    options.stderr_fd = STDERR_FILENO;

    pid = psx_spawn_ex(&options, &failure);

    if(pid < 0) {
        printf("PSXTERM-EXIT %d (spawn failed at %s: %s)\n", -1,
               psx_spawn_stage_name(failure.stage), failure.detail);
        return 1;
    }

    if(psx_process_wait(pid, &status, 30000) != 0) {
        psx_process_kill(pid, SIGKILL);
        status = -1;
    }

    if(status >= 0 && WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    } else if(status >= 0 && WIFSIGNALED(status)) {
        status = 128 + WTERMSIG(status);
    }

    printf("PSXTERM-EXIT %d\n", status);
    fflush(stdout);

    return 0;
}
