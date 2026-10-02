#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/instance.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/util.h"

#define PSX_INSTANCE_MAX_PROCS 256
#define PSX_INSTANCE_PATH_MAX 512
#define PSX_INSTANCE_REPLACE_LIMIT 8
#define PSX_INSTANCE_TERM_WAIT_MS 1000
#define PSX_INSTANCE_POLL_MS 50

void
psx_instance_pid_path(char *out, size_t out_cap)
{
    const char *override = getenv("PSXTERM_PID_FILE");

    if(override && *override) {
        snprintf(out, out_cap, "%s", override);
        return;
    }

    snprintf(out, out_cap, "%s/psxtermd.pid", psx_platform_home_dir());
}

static bool
process_alive(int32_t pid)
{
    if(pid <= 1) {
        return false;
    }

    return kill((pid_t)pid, 0) == 0 || errno != ESRCH;
}

static void
sleep_ms(unsigned ms)
{
    struct timespec pause = {.tv_sec = ms / 1000,
                             .tv_nsec = (long)(ms % 1000) * 1000000L};

    nanosleep(&pause, NULL);
}

static bool
find_process_name(int32_t pid, char *name, size_t name_cap)
{
    psx_proc_entry_t entries[PSX_INSTANCE_MAX_PROCS];
    int count = psx_platform_list_processes(entries, PSX_INSTANCE_MAX_PROCS);

    if(count <= 0) {
        return false;
    }

    for(int i = 0; i < count; i++) {
        if(entries[i].pid == pid) {
            snprintf(name, name_cap, "%s", entries[i].name);
            return true;
        }
    }

    return false;
}

/*
 * Names that must never be replacement targets: Sony system processes share
 * the "Sce" prefix, and an empty name means the snapshot entry is unusable.
 * If our own process reports such a name the sibling fallback is disabled
 * entirely, because payloads could not be told apart from system services.
 */
static bool
is_protected_name(const char *name)
{
    if(!name[0]) {
        return true;
    }

    if(strncmp(name, "Sce", 3) == 0) {
        return true;
    }

    if(strncmp(name, "kernel", 6) == 0) {
        return true;
    }

    return false;
}

void
psx_instance_own_name(char *out, size_t out_cap)
{
    out[0] = '\0';
    find_process_name((int32_t)getpid(), out, out_cap);
}

static bool
own_process_name(char *name, size_t name_cap)
{
    psx_instance_own_name(name, name_cap);

    return name[0] != '\0';
}

/* SIGTERM, wait, then SIGKILL - the pattern the reference instance manager
 * uses. Never signals pid <= 1 or ourselves. */
static bool
terminate_process(int32_t pid)
{
    if(pid <= 1 || pid == (int32_t)getpid()) {
        return false;
    }

    PSX_LOGI("instance: sending SIGTERM to pid %d", pid);

    if(kill((pid_t)pid, SIGTERM) != 0) {
        PSX_LOGW("instance: SIGTERM to pid %d failed: %s", pid,
                 strerror(errno));
        return !process_alive(pid);
    }

    for(int waited = 0; waited < PSX_INSTANCE_TERM_WAIT_MS;
        waited += PSX_INSTANCE_POLL_MS) {
        if(!process_alive(pid)) {
            PSX_LOGI("instance: pid %d exited", pid);
            return true;
        }
        sleep_ms(PSX_INSTANCE_POLL_MS);
    }

    PSX_LOGW("instance: pid %d ignored SIGTERM, escalating to SIGKILL", pid);
    kill((pid_t)pid, SIGKILL);

    for(int waited = 0; waited < PSX_INSTANCE_TERM_WAIT_MS;
        waited += PSX_INSTANCE_POLL_MS) {
        if(!process_alive(pid)) {
            PSX_LOGI("instance: pid %d killed", pid);
            return true;
        }
        sleep_ms(PSX_INSTANCE_POLL_MS);
    }

    PSX_LOGW("instance: pid %d still alive after SIGKILL", pid);

    return false;
}

int
psx_instance_replace_siblings(void)
{
    psx_proc_entry_t entries[PSX_INSTANCE_MAX_PROCS];
    char own[sizeof(entries[0].name)];
    int replaced = 0;
    int count;

    if(!own_process_name(own, sizeof(own))) {
        PSX_LOGW("instance: cannot determine our own process name; not "
                 "replacing siblings");
        return 0;
    }

    if(is_protected_name(own)) {
        PSX_LOGW("instance: ours reads as '%s' (system-like); refusing to "
                 "replace siblings", own);
        return 0;
    }

    if((count = psx_platform_list_processes(entries, PSX_INSTANCE_MAX_PROCS)) <=
       0) {
        PSX_LOGD("instance: process enumeration unavailable; relying on the "
                 "pid file only");
        return 0;
    }

    for(int i = 0; i < count && replaced < PSX_INSTANCE_REPLACE_LIMIT; i++) {
        if(entries[i].pid <= 1 || entries[i].pid == (int32_t)getpid()) {
            continue;
        }

        if(strcmp(entries[i].name, own) != 0 ||
           is_protected_name(entries[i].name)) {
            continue;
        }

        PSX_LOGW("instance: replacing previous instance pid %d (%s)",
                 entries[i].pid, entries[i].name);

        if(terminate_process(entries[i].pid)) {
            replaced++;
        }
    }

    return replaced;
}

int
psx_instance_claim(void)
{
    char path[PSX_INSTANCE_PATH_MAX];
    char own[64];
    int32_t recorded_pid = 0;
    bool replaced = false;
    bool enumerable;
    FILE *fp;

    psx_instance_pid_path(path, sizeof(path));

    if((fp = fopen(path, "r"))) {
        if(fscanf(fp, "%d", &recorded_pid) != 1) {
            recorded_pid = 0;
        }
        fclose(fp);
    }

    enumerable = own_process_name(own, sizeof(own));

    if(recorded_pid > 1 && recorded_pid != (int32_t)getpid() &&
       process_alive(recorded_pid)) {
        char recorded_name[64] = "";

        if(enumerable && !is_protected_name(own) &&
           find_process_name(recorded_pid, recorded_name,
                             sizeof(recorded_name)) &&
           strcmp(recorded_name, own) == 0) {
            PSX_LOGI("instance: previous instance pid %d recorded in %s",
                     recorded_pid, path);
            replaced = terminate_process(recorded_pid);
        } else {
            PSX_LOGW("instance: %s names pid %d (%s), refusing to signal it",
                     path, recorded_pid,
                     recorded_name[0] ? recorded_name : "unknown");
        }
    }

    if((fp = fopen(path, "w"))) {
        fprintf(fp, "%d\n", (int)getpid());
        fclose(fp);
    } else {
        PSX_LOGW("instance: cannot write %s: %s", path, strerror(errno));
        return -1;
    }

    PSX_LOGD("instance: pid file %s updated (pid %d)", path, (int)getpid());

    return replaced ? 0 : 1;
}

void
psx_instance_release(void)
{
    char path[PSX_INSTANCE_PATH_MAX];
    int32_t recorded_pid = 0;
    FILE *fp;

    psx_instance_pid_path(path, sizeof(path));

    if(!(fp = fopen(path, "r"))) {
        return;
    }

    if(fscanf(fp, "%d", &recorded_pid) != 1) {
        recorded_pid = 0;
    }
    fclose(fp);

    if(recorded_pid == (int32_t)getpid()) {
        unlink(path);
    }
}
