#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <ps4/kernel.h>
#include <ps4/klog.h>

#include "psxterm/instance.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"

/*
 * PS4 process enumeration is not implemented yet: the kinfo_proc layout of
 * this firmware is unverified, and guessing it could make the daemon signal
 * unrelated processes. PS4 therefore relies on the pid file alone
 * (HARDWARE TEST REQUIRED for the replacement path).
 */
int
psx_platform_list_processes(psx_proc_entry_t *entries, size_t max_entries)
{
    (void)entries;
    (void)max_entries;

    return 0;
}

/* PSXTerm on PS4: payload-side platform primitives. */

bool
psx_platform_init(void)
{
    psx_privilege_raise();

    return psx_fs_prepare();
}

/*
 * PS4 payloads are hosted by GoldHEN's long-lived loader process: rewriting
 * its authid/prison/root vnodes would change the execution context used by
 * system notifications and later ELF launches, so this stays untouched (the
 * reference privilege manager does the same on PS4). Only the filesystem
 * preparation below touches the sandbox.
 */
bool
psx_privilege_raise(void)
{
    return true;
}

const char *
psx_platform_name(void)
{
    return "PS4";
}

const char *
psx_platform_id(void)
{
    return "ps4";
}

const char *
psx_platform_uname(void)
{
    return "FreeBSD 9 (PS4 payload)";
}

const char *
psx_platform_firmware(void)
{
    /*
     * The PS4 firmware version is not exposed through a reliable payload-side
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
    return NULL;
}

void
psx_platform_log_line(const char *line)
{
    klog_printf("%s", line);
    klog_printf("%s", "\n");
}

bool
psx_platform_is_target(void)
{
    return true;
}
