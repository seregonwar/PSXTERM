#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psxterm/shell.h"

int
psh_builtin_pwd(psx_session_t *session, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return psh_out(session, "%s\n", session->cwd) < 0 ? 1 : 0;
}

int
psh_builtin_cd(psx_session_t *session, int argc, char **argv)
{
    const char *target;
    char path[PSX_PATH_MAX];
    struct stat st;

    if(argc > 2) {
        psh_err(session, "cd: too many arguments\n");
        return 1;
    }

    target = argc > 1 ? argv[1] : psx_env_get(&session->env, "HOME");
    if(!target || !*target) {
        psh_err(session, "cd: HOME not set\n");
        return 1;
    }

    if(psx_session_absolute_path(session, target, path, sizeof(path)) < 0) {
        psh_err(session, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if(stat(path, &st) != 0) {
        psh_err(session, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if(!S_ISDIR(st.st_mode)) {
        psh_err(session, "cd: %s: not a directory\n", target);
        return 1;
    }

    snprintf(session->cwd, sizeof(session->cwd), "%s", path);
    psx_env_set(&session->env, "PWD", session->cwd);

    return 0;
}

static int
compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int
psh_builtin_ls(psx_session_t *session, int argc, char **argv)
{
    const char *target = argc > 1 ? argv[1] : ".";
    char path[PSX_PATH_MAX];
    char full[PSX_PATH_MAX];
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    DIR *dir;
    struct dirent *entry;
    int status = 0;

    if(argc > 2) {
        psh_err(session, "ls: too many arguments\n");
        return 1;
    }

    if(psx_session_absolute_path(session, target, path, sizeof(path)) < 0) {
        psh_err(session, "ls: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if(!(dir = opendir(path))) {
        psh_err(session, "ls: %s: %s\n", target, strerror(errno));
        return 1;
    }

    while((entry = readdir(dir))) {
        char **grown;

        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if(count == cap) {
            cap = cap ? cap * 2 : 32;
            if(!(grown = realloc(names, cap * sizeof(char *)))) {
                status = 1;
                break;
            }
            names = grown;
        }

        if(!(names[count] = strdup(entry->d_name))) {
            status = 1;
            break;
        }
        count++;
    }

    closedir(dir);

    qsort(names, count, sizeof(char *), compare_names);

    for(size_t i = 0; i < count; i++) {
        struct stat st;

        if(psx_path_join(full, sizeof(full), path, names[i]) == 0 &&
           stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            psh_out(session, "%s/\n", names[i]);
        } else {
            psh_out(session, "%s\n", names[i]);
        }
        free(names[i]);
    }

    free(names);

    return status;
}

int
psh_builtin_cat(psx_session_t *session, int argc, char **argv)
{
    uint8_t buffer[4096];
    int status = 0;

    if(argc < 2) {
        psh_err(session, "cat: missing file operand\n");
        return 1;
    }

    for(int i = 1; i < argc; i++) {
        char path[PSX_PATH_MAX];
        int fd;

        if(psx_session_absolute_path(session, argv[i], path, sizeof(path)) < 0) {
            psh_err(session, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if((fd = open(path, O_RDONLY)) < 0) {
            psh_err(session, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        for(;;) {
            ssize_t n = read(fd, buffer, sizeof(buffer));

            if(n < 0) {
                if(errno == EINTR) {
                    continue;
                }
                psh_err(session, "cat: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                break;
            }

            if(n == 0) {
                break;
            }

            if(psx_session_emit(session, PTTY_MSG_STDOUT, buffer, (size_t)n) < 0) {
                status = 1;
                break;
            }
        }

        close(fd);
    }

    return status;
}
