#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

static bool
ensure_dir(const char *path)
{
    struct stat st;

    if(stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if(mkdir(path, 0755) == 0) {
        return true;
    }

    return errno == EEXIST;
}

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

    if(!ensure_dir("/data/psxterm")) {
        PSX_LOGW("fs: cannot create /data/psxterm: %s", strerror(errno));
        return false;
    }

    if(!ensure_dir("/data/psxterm/bin")) {
        PSX_LOGW("fs: cannot create /data/psxterm/bin: %s", strerror(errno));
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
