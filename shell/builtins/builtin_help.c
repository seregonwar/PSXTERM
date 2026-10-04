#include <string.h>

#include "psxterm/shell.h"

const char *
psh_command_usage(const char *name)
{
    static const struct {
        const char *name;
        const char *text;
    } usage[] = {
        {"help", "usage: help [command ...]\nShow built-in commands or "
                 "detailed command usage."},
        {"pwd", "usage: pwd\nPrint the session working directory."},
        {"cd", "usage: cd [--] [directory|-]\nChange directory; default is "
               "HOME. '-' returns to OLDPWD."},
        {"ls", "usage: ls [-aA1CxlhnFrStpd] [--color=WHEN] [--] [path "
               "...]\nList files; --help describes listing options."},
        {"cat",
         "usage: cat [-nbsvETA] [--] file ...\nConcatenate files. -n numbers "
         "lines; -b numbers nonempty lines;\n-s squeezes blank lines; -E shows "
         "line ends; -T shows tabs;\n-v shows control bytes; -A is equivalent "
         "to -vET. Session stdin is not supported."},
        {"clear", "usage: clear\nClear the visible terminal screen and move "
                  "the cursor home."},
        {"env", "usage: env [-0|--null]\nPrint the sorted session environment; "
                "-0 terminates entries with NUL."},
        {"export", "usage: export [-p] [NAME=value ...]\nSet session "
                   "variables; no operands or -p prints quoted assignments."},
        {"unset", "usage: unset [--] NAME ...\nRemove session variables."},
        {"uname", "usage: uname [-a]\nPrint the PSXTerm platform, release and "
                  "architecture description."},
        {"whoami", "usage: whoami\nPrint the session user name."},
        {"ps", "usage: ps\nShow PSXTerm sessions and foreground processes (not "
               "a system process list)."},
        {"exit",
         "usage: exit [status]\nClose the session with a numeric exit status."},
    };
    for(size_t i = 0; i < sizeof(usage) / sizeof(usage[0]); i++) {
        if(strcmp(name, usage[i].name) == 0) {
            return usage[i].text;
        }
    }
    return NULL;
}

int
psh_builtin_help(psx_session_t *session, int argc, char **argv)
{
    size_t count = 0;
    const psh_command_t *commands = psh_registry_table(&count);

    int status = 0;

    if(argc > 1) {
        for(int i = 1; i < argc; i++) {
            const char *usage = psh_command_usage(
                strcmp(argv[i], "--help") == 0 ? "help" : argv[i]);
            if(!usage) {
                psh_err(session, "help: '%s': no such built-in command\n",
                        argv[i]);
                status = 1;
            } else if(psh_out(session, "%s%s\n", i > 1 ? "\n" : "", usage) <
                      0) {
                return 1;
            }
        }
        return status;
    }

    if(psh_out(session, "psh built-in commands:\n") < 0)
        return 1;
    for(size_t i = 0; i < count; i++) {
        if(psh_out(session, "  %-10s %s\n", commands[i].name,
                   commands[i].description) < 0) {
            return 1;
        }
    }
    return psh_out(session,
                   "Use 'help COMMAND' or 'COMMAND --help' for usage.\n"
                   "Other commands are resolved through PATH.\n") < 0
               ? 1
               : 0;
}
