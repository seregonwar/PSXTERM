#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "psxterm/platform.h"

bool
psx_platform_init(void)
{
    return true;
}

const char *
psx_platform_name(void)
{
    return "Host";
}

const char *
psx_platform_id(void)
{
    return "host";
}

const char *
psx_platform_uname(void)
{
    static char buf[256];
    struct utsname u;

    if(uname(&u) == 0) {
        snprintf(buf, sizeof(buf), "%s %s %s", u.sysname, u.release, u.machine);
        return buf;
    }

    return "Host (unknown)";
}

const char *
psx_platform_default_path(void)
{
    const char *path = getenv("PATH");

    return path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
}

const char *
psx_platform_bin_dir(void)
{
    return "bin";
}

int
psx_platform_random_bytes(void *buf, size_t len)
{
    FILE *fp;

    if(!(fp = fopen("/dev/urandom", "rb"))) {
        return -1;
    }

    if(fread(buf, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    return 0;
}

bool
psx_platform_is_target(void)
{
    return false;
}
