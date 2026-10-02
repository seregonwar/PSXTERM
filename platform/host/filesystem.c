#include <stdlib.h>
#include <unistd.h>

#include "psxterm/platform.h"

/*
 * Host filesystem preparation: nothing to do; the client and daemon run with
 * ordinary POSIX permissions.
 */

bool
psx_fs_prepare(void)
{
    return true;
}

const char *
psx_fs_default_cwd(void)
{
    const char *home = getenv("HOME");

    return home && *home ? home : "/";
}
