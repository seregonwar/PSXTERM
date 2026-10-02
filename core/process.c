#include <errno.h>
#include <string.h>

#include "psxterm/process.h"

pid_t
psx_spawn(const psx_spawn_options_t *options)
{
    if(!options || !options->path || !*options->path) {
        errno = EINVAL;
        return -1;
    }

    if(!options->argv || !options->argv[0]) {
        errno = EINVAL;
        return -1;
    }

    return psx_platform_spawn(options);
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
