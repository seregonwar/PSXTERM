/*
 * No console loader on the host: the injected-spawn path is the only one, and
 * it is a normal fork/exec here anyway.
 */

#include <errno.h>

#include "psxterm/platform.h"

int
psx_platform_loader_exec(const char *command_line)
{
    (void)command_line;
    errno = ENOSYS;

    return -1;
}
