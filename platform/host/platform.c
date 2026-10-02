#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "psxterm/instance.h"
#include "psxterm/platform.h"

/* Development-host process snapshot (Linux /proc; used by instance tests). */
int
psx_platform_list_processes(psx_proc_entry_t *entries, size_t max_entries)
{
    DIR *dir;
    struct dirent *entry;
    size_t count = 0;

    if(!entries || max_entries == 0) {
        return -1;
    }

    if(!(dir = opendir("/proc"))) {
        return -1;
    }

    while((entry = readdir(dir)) && count < max_entries) {
        char path[64];
        char line[64];
        int pid = atoi(entry->d_name);
        FILE *fp;

        if(pid <= 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        if(!(fp = fopen(path, "r"))) {
            continue;
        }

        line[0] = '\0';
        if(!fgets(line, sizeof(line), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);
        line[strcspn(line, "\r\n")] = '\0';

        entries[count].pid = pid;
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", line);
        count++;
    }

    closedir(dir);

    return (int)count;
}

bool
psx_platform_init(void)
{
    return psx_fs_prepare();
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
psx_platform_firmware(void)
{
    /* A development host has no console firmware to report. */
    return NULL;
}

const char *
psx_platform_default_path(void)
{
    const char *path = getenv("PATH");

    return path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
}

const char *
psx_platform_home_dir(void)
{
    const char *home = getenv("HOME");

    return home && *home ? home : "/tmp";
}

const char *
psx_platform_user_name(void)
{
    const char *user = getenv("USER");

    return user && *user ? user : "user";
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

char *const *
psx_platform_inherit_environ(void)
{
    extern char **environ;

    return environ;
}

void
psx_platform_log_line(const char *line)
{
    /* The development host has a real stderr; nothing extra to do. */
    (void)line;
}

bool
psx_platform_is_target(void)
{
    return false;
}
