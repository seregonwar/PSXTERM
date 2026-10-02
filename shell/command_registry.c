#include <string.h>

#include "psxterm/shell.h"

/*
 * Builtin command registry. Each builtin lives in its own translation unit
 * under shell/builtins/ and is registered here.
 */

static const psh_command_t psh_commands[] = {
    {"help", "list available commands", psh_builtin_help},
    {"pwd", "print the current working directory", psh_builtin_pwd},
    {"cd", "change the current working directory", psh_builtin_cd},
    {"ls", "list directory contents", psh_builtin_ls},
    {"cat", "print file contents", psh_builtin_cat},
    {"clear", "clear the terminal screen", psh_builtin_clear},
    {"env", "print the session environment", psh_builtin_env},
    {"export", "set an environment variable", psh_builtin_export},
    {"unset", "remove an environment variable", psh_builtin_unset},
    {"uname", "print platform information", psh_builtin_uname},
    {"whoami", "print the current user name", psh_builtin_whoami},
    {"ps", "list PSXTerm sessions and processes", psh_builtin_ps},
    {"exit", "close the session", psh_builtin_exit},
};

static const size_t psh_command_count =
    sizeof(psh_commands) / sizeof(psh_commands[0]);

const psh_command_t *
psh_registry_lookup(const char *name)
{
    for(size_t i = 0; i < psh_command_count; i++) {
        if(strcmp(psh_commands[i].name, name) == 0) {
            return &psh_commands[i];
        }
    }

    return NULL;
}

const psh_command_t *
psh_registry_table(size_t *count)
{
    if(count) {
        *count = psh_command_count;
    }

    return psh_commands;
}
