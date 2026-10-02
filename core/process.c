#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "psxterm/process.h"

const char *
psx_spawn_stage_name(psx_spawn_stage_t stage)
{
    switch(stage) {
    case PSX_SPAWN_STAGE_PREPARE: return "PREPARE";
    case PSX_SPAWN_STAGE_CREATE_VICTIM: return "CREATE_VICTIM";
    case PSX_SPAWN_STAGE_ATTACH: return "ATTACH";
    case PSX_SPAWN_STAGE_RAISE_PRIVILEGES: return "RAISE_PRIVILEGES";
    case PSX_SPAWN_STAGE_DUP_STDIO: return "DUP_STDIO";
    case PSX_SPAWN_STAGE_LOAD_ELF: return "LOAD_ELF";
    case PSX_SPAWN_STAGE_RELOCATE: return "RELOCATE";
    case PSX_SPAWN_STAGE_SET_REGISTERS: return "SET_REGISTERS";
    case PSX_SPAWN_STAGE_DETACH: return "DETACH";
    case PSX_SPAWN_STAGE_RUNNING: return "RUNNING";
    default: return "UNKNOWN";
    }
}

pid_t
psx_spawn_ex(const psx_spawn_options_t *options, psx_spawn_failure_t *failure)
{
    psx_spawn_failure_t local;
    psx_spawn_options_t bounded;
    char *argv_fixed[PSX_SPAWN_MAX_ARGS + 1];
    char *envp_fixed[PSX_SPAWN_MAX_ENV + 1];

    if(!failure) {
        failure = &local;
    }

    memset(failure, 0, sizeof(*failure));
    failure->stage = PSX_SPAWN_STAGE_PREPARE;

    if(!options || !options->path || !*options->path) {
        failure->error_code = EINVAL;
        snprintf(failure->detail, sizeof(failure->detail), "%s",
                 "empty executable path");
        errno = EINVAL;
        return -1;
    }

    if(!options->argv || !options->argv[0]) {
        failure->error_code = EINVAL;
        snprintf(failure->detail, sizeof(failure->detail), "%s",
                 "missing argv");
        errno = EINVAL;
        return -1;
    }

    /*
     * Bounded, guaranteed NUL-terminated copies of argv and envp. A caller
     * that forgets the terminator makes the kernel read past the array, and
     * the payload starts with a nonsense argument count and garbage argument
     * pointers (hardware-verified: argc=50 and unreadable argv strings).
     */
    bounded = *options;

    {
        size_t n = 0;

        while(n < PSX_SPAWN_MAX_ARGS && options->argv[n]) {
            argv_fixed[n] = options->argv[n];
            n++;
        }
        argv_fixed[n] = NULL;
        bounded.argv = argv_fixed;
    }

    if(options->envp) {
        size_t n = 0;

        while(n < PSX_SPAWN_MAX_ENV && options->envp[n]) {
            envp_fixed[n] = options->envp[n];
            n++;
        }
        envp_fixed[n] = NULL;
        bounded.envp = envp_fixed;
    }

    return psx_platform_spawn(&bounded, failure);
}

pid_t
psx_spawn(const psx_spawn_options_t *options)
{
    return psx_spawn_ex(options, NULL);
}

int
psx_process_wait(pid_t pid, int *status_out, int timeout_ms)
{
    return psx_platform_process_wait(pid, status_out, timeout_ms);
}

int
psx_process_kill(pid_t pid, int sig)
{
    return psx_platform_process_kill(pid, sig);
}

bool
psx_process_backend_available(void)
{
    return psx_platform_process_available();
}

const char *
psx_process_backend_name(void)
{
    return psx_platform_process_name();
}
