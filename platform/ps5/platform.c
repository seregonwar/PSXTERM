#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <ps5/kernel.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

/* PSXTerm on PS5: payload-side platform primitives. */

bool
psx_platform_init(void)
{
    return psx_fs_prepare();
}

const char *
psx_platform_name(void)
{
    return "PS5";
}

const char *
psx_platform_id(void)
{
    return "ps5";
}

const char *
psx_platform_uname(void)
{
    return "FreeBSD 11 (PS5 payload)";
}

const char *
psx_platform_firmware(void)
{
    /*
     * The PS5 firmware version is not exposed through a reliable payload-side
     * interface; reporting a guessed value would be worse than UNKNOWN.
     */
    return NULL;
}

const char *
psx_platform_default_path(void)
{
    return "/data/psxterm/bin";
}

const char *
psx_platform_home_dir(void)
{
    return "/data/psxterm";
}

const char *
psx_platform_user_name(void)
{
    /* After the usual payload privilege raise the effective uid is 0. */
    return "root";
}

const char *
psx_platform_bin_dir(void)
{
    return "/data/psxterm/bin";
}

int
psx_platform_random_bytes(void *buf, size_t len)
{
    arc4random_buf(buf, len);

    return 0;
}

char *const *
psx_platform_inherit_environ(void)
{
    /* A payload starts from the process it was injected into; PSXTerm builds
     * session environments from its own defaults instead. */
    return NULL;
}

bool
psx_platform_is_target(void)
{
    return true;
}
