/*
 * PS4 has its own loader (GoldHEN's binloader) and PSXTerm does not drive it
 * yet: the injected-spawn path stays the only one there.
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
