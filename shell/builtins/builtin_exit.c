#include <stdlib.h>
#include <string.h>

#include "psxterm/shell.h"
#include "psxterm/util.h"

int
psh_builtin_exit(psx_session_t *session, int argc, char **argv)
{
    int code = 0;

    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
        return psh_out(session, "%s\n", psh_command_usage("exit")) < 0 ? 1 : 0;
    }

    if(argc > 2) {
        psh_err(session, "exit: too many arguments\n");
        return 1;
    }

    if(argc == 2) {
        if(!psx_parse_int(argv[1], &code)) {
            psh_err(session, "exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        }
    }

    if(session->shell) {
        psh_shell_request_exit(session->shell, code);
    }

    return code;
}
