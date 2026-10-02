#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "psxterm/instance.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"

/*
 * Process snapshot through the same sysctl(KERN_PROC_PROC) interface the
 * reference loaders use.
 *
 * Hardware finding: the SDK's struct kinfo_proc does NOT match the kernel
 * record layout in this environment - offsetof(ki_tdname) resolves to the main
 * thread name of the hijacked Sony process ("SceSpZeroConfMai"), while the
 * loader-assigned payload name ("payload.elf") sits at byte 447. The reference
 * ELF loaders use those absolute offsets for exactly that reason, so PSXTerm
 * does the same and validates every field it reads.
 */
#define PSX_KINFO_PID_OFFSET 72
#define PSX_KINFO_NAME_OFFSET 447
#define PSX_KINFO_NAME_SIZE 20

static bool
printable_name(const char *name)
{
    for(size_t i = 0; i < PSX_KINFO_NAME_SIZE; i++) {
        unsigned char c = (unsigned char)name[i];

        if(c == '\0') {
            return i > 0;
        }
        if(c < 0x20 || c > 0x7e) {
            return false;
        }
    }

    /* Not terminated inside the field: do not trust it. */
    return false;
}

int
psx_platform_list_processes(psx_proc_entry_t *entries, size_t max_entries)
{
    const int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;
    uint8_t *buf;
    size_t count = 0;

    if(!entries || max_entries == 0) {
        return -1;
    }

    if(sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0 || buf_size == 0) {
        return -1;
    }

    if(!(buf = malloc(buf_size))) {
        return -1;
    }

    if(sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
        free(buf);
        return -1;
    }

    for(const uint8_t *ptr = buf; count < max_entries;) {
        const char *name;
        int record_size;

        if(ptr + PSX_KINFO_NAME_OFFSET + PSX_KINFO_NAME_SIZE > buf + buf_size) {
            break;
        }

        record_size = *(const int *)(const void *)ptr;
        if(record_size <= 0 || ptr + record_size > buf + buf_size) {
            break;
        }

        name = (const char *)ptr + PSX_KINFO_NAME_OFFSET;
        if(printable_name(name)) {
            entries[count].pid = *(const int32_t *)(const void *)(ptr +
                                                                  PSX_KINFO_PID_OFFSET);
            snprintf(entries[count].name, sizeof(entries[count].name), "%s",
                     name);
            count++;
        }

        ptr += record_size;
    }

    free(buf);

    return (int)count;
}

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
