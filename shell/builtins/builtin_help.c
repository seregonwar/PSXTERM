#include "psxterm/shell.h"

int
psh_builtin_help(psx_session_t *session, int argc, char **argv)
{
    size_t count = 0;
    const psh_command_t *commands = psh_registry_table(&count);

    (void)argc;
    (void)argv;

    psh_out(session, "psh built-in commands:\n");
    for(size_t i = 0; i < count; i++) {
        psh_out(session, "  %-10s %s\n", commands[i].name,
                commands[i].description);
    }
    psh_out(session, "Other commands are resolved through PATH.\n");

    return 0;
}
