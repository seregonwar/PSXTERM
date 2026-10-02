#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

/*
 * PS4 filesystem preparation.
 *
 * The reference loaders raise the process out of its sandbox before jumping
 * into a payload; PSXTerm retries the same sequence defensively in case it
 * was started by a less privileged loader.
 *
 * HARDWARE TEST REQUIRED: this has not been executed on a PS4.
 */

bool
psx_fs_prepare(void)
{
    if(access("/data", W_OK) == 0) {
        return true;
    }

    if(kernel_set_proc_rootdir(getpid(), KERNEL_ADDRESS_ROOTVNODE) != 0 ||
       kernel_set_proc_jaildir(getpid(), KERNEL_ADDRESS_ROOTVNODE) != 0 ||
       kernel_set_ucred_prison(getpid(), KERNEL_ADDRESS_PRISON0) != 0 ||
       kernel_set_ucred_uid(getpid(), 0) != 0) {
        PSX_LOGW("fs: privilege raise failed: %s", strerror(errno));
        return false;
    }

    if(access("/data", W_OK) != 0) {
        PSX_LOGW("fs: /data still not writable; using the sandboxed root");
        return false;
    }

    return true;
}

const char *
psx_fs_default_cwd(void)
{
    if(access("/data", W_OK) == 0) {
        return "/data/psxterm";
    }

    return "/";
}
