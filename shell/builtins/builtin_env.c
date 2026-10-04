#include <stdlib.h>
#include <string.h>

#include "psxterm/shell.h"

static bool
valid_name(const char *name, size_t length)
{
    if(!length)
        return false;
    for(size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if(c != '_' && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
           !(i && c >= '0' && c <= '9'))
            return false;
    }
    return true;
}

static int
entry_compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
env_print(psx_session_t *session, bool nul, bool assignments)
{
    const char *entries[PSX_ENV_MAX];
    size_t count = psx_env_count(&session->env);
    for(size_t i = 0; i < count; i++)
        entries[i] = psx_env_entry(&session->env, i);
    qsort(entries, count, sizeof(*entries), entry_compare);
    for(size_t i = 0; i < count; i++) {
        int rc;
        if(assignments) {
            const char *equals = strchr(entries[i], '=');
            char *value = psh_display_text(equals + 1, true, NULL);
            if(!value)
                return 1;
            rc = psh_out(session, "export %.*s=%s\n",
                         (int)(equals - entries[i]), entries[i], value);
            free(value);
        } else if(nul) {
            rc = psx_session_emit(session, PTTY_MSG_STDOUT, entries[i],
                                  strlen(entries[i]) + 1);
        } else
            rc = psh_out(session, "%s\n", entries[i]);
        if(rc < 0)
            return 1;
    }
    return 0;
}

int
psh_builtin_env(psx_session_t *session, int argc, char **argv)
{
    if(argc == 2 &&
       (strcmp(argv[1], "-0") == 0 || strcmp(argv[1], "--null") == 0)) {
        return env_print(session, true, false);
    }
    int check = psh_no_arguments(session, argc, argv);
    return check >= 0 ? check : env_print(session, false, false);
}

int
psh_builtin_export(psx_session_t *session, int argc, char **argv)
{
    int status = 0;
    if(argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        return env_print(session, false, true);
    }
    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
        return psh_out(session, "%s\n", psh_command_usage("export")) < 0 ? 1
                                                                         : 0;
    }
    for(int i = 1; i < argc; i++) {
        const char *equals = strchr(argv[i], '=');
        size_t length = equals ? (size_t)(equals - argv[i]) : strlen(argv[i]);
        if(!valid_name(argv[i], length)) {
            char *name = psh_display_text(argv[i], true, NULL);
            psh_err(session, "export: %s: not a valid identifier\n",
                    name ? name : "?");
            free(name);
            status = 1;
        } else if(equals) {
            if(psx_env_set_entry(&session->env, argv[i]) < 0) {
                psh_err(session, "export: %.*s: cannot set variable\n",
                        (int)length, argv[i]);
                status = 1;
            }
        } else if(!psx_env_get(&session->env, argv[i])) {
            if(psx_env_set(&session->env, argv[i], "") < 0) {
                psh_err(session, "export: %s: cannot set variable\n", argv[i]);
                status = 1;
            }
        }
    }
    return status;
}

int
psh_builtin_unset(psx_session_t *session, int argc, char **argv)
{
    int first = 1, status = 0;
    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
        return psh_out(session, "%s\n", psh_command_usage("unset")) < 0 ? 1 : 0;
    }
    if(argc > 1 && strcmp(argv[1], "--") == 0)
        first++;
    if(argc == first) {
        psh_err(session, "unset: missing variable name\n");
        return 1;
    }
    for(int i = first; i < argc; i++) {
        if(!valid_name(argv[i], strlen(argv[i]))) {
            char *name = psh_display_text(argv[i], true, NULL);
            psh_err(session, "unset: %s: not a valid identifier\n",
                    name ? name : "?");
            free(name);
            status = 1;
        } else
            psx_env_unset(&session->env, argv[i]);
    }
    return status;
}
