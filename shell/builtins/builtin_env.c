#include <string.h>

#include "psxterm/shell.h"

int
psh_builtin_env(psx_session_t *session, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for(size_t i = 0; i < psx_env_count(&session->env); i++) {
        psh_out(session, "%s\n", psx_env_entry(&session->env, i));
    }

    return 0;
}

int
psh_builtin_export(psx_session_t *session, int argc, char **argv)
{
    int status = 0;

    if(argc < 2) {
        return psh_builtin_env(session, argc, argv);
    }

    for(int i = 1; i < argc; i++) {
        if(strchr(argv[i], '=')) {
            if(psx_env_set_entry(&session->env, argv[i]) < 0) {
                psh_err(session, "export: %s: cannot set variable\n", argv[i]);
                status = 1;
            }
        } else if(!psx_env_get(&session->env, argv[i])) {
            psh_err(session, "export: %s: not set\n", argv[i]);
            status = 1;
        }
    }

    return status;
}

int
psh_builtin_unset(psx_session_t *session, int argc, char **argv)
{
    if(argc < 2) {
        psh_err(session, "unset: missing variable name\n");
        return 1;
    }

    for(int i = 1; i < argc; i++) {
        psx_env_unset(&session->env, argv[i]);
    }

    return 0;
}
