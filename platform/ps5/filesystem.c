#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

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
 * PS5 filesystem preparation.
 *
 * A payload injected by an ELF loader usually already has the sandbox lifted;
 * PSXTerm retries the same privileged sequence defensively so that /data is
 * reachable when it was launched from a less privileged context.
 *
 * HARDWARE TEST REQUIRED: this has not been executed on a PS5.
 */

bool
psx_fs_prepare(void)
{
    intptr_t vnode;

    if(access("/data", W_OK) != 0) {
        if(!(vnode = kernel_get_root_vnode())) {
            PSX_LOGW("fs: kernel_get_root_vnode unavailable; staying in the "
                     "current sandbox");
            return false;
        }

        if(kernel_set_proc_rootdir(getpid(), vnode) != 0 ||
           kernel_set_proc_jaildir(getpid(), 0) != 0 ||
           kernel_set_ucred_uid(getpid(), 0) != 0) {
            PSX_LOGW("fs: privilege raise failed: %s", strerror(errno));
            return false;
        }

        if(access("/data", W_OK) != 0) {
            PSX_LOGW("fs: /data still not writable; using the sandboxed root");
            return false;
        }
    }

    /*
     * Created unconditionally: hardware bring-up showed that a writable /data
     * says nothing about PSXTerm's own directories existing, and the pid file
     * lives there.
     */
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
