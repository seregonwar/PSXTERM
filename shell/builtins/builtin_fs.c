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
    int check = psh_no_arguments(session, argc, argv);
    if(check >= 0)
        return check;

    return psh_out(session, "%s\n", session->cwd) < 0 ? 1 : 0;
}

int
psh_builtin_cd(psx_session_t *session, int argc, char **argv)
{
    const char *target;
    char path[PSX_PATH_MAX];
    struct stat st;
    int first = 1;
    bool previous = false;

    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
        return psh_out(session, "%s\n", psh_command_usage("cd")) < 0 ? 1 : 0;
    }
    if(argc > 1 && strcmp(argv[1], "--") == 0)
        first++;

    if(argc - first > 1) {
        psh_err(session, "cd: too many arguments\n");
        return 1;
    }

    target = argc > first ? argv[first] : psx_env_get(&session->env, "HOME");
    if(target && strcmp(target, "-") == 0) {
        target = psx_env_get(&session->env, "OLDPWD");
        previous = true;
    }
    if(!target || !*target) {
        psh_err(session, "cd: %s not set\n", previous ? "OLDPWD" : "HOME");
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

    if(psx_env_set(&session->env, "OLDPWD", session->cwd) < 0 ||
       psx_env_set(&session->env, "PWD", path) < 0) {
        psh_err(session, "cd: cannot update session environment\n");
        return 1;
    }
    snprintf(session->cwd, sizeof(session->cwd), "%s", path);
    return previous && psh_out(session, "%s\n", session->cwd) < 0 ? 1 : 0;
}
