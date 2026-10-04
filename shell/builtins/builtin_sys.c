#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psxterm/platform.h"
#include "psxterm/shell.h"
#include "psxterm/version.h"

int
psh_builtin_uname(psx_session_t *session, int argc, char **argv)
{
    if(!(argc == 2 && strcmp(argv[1], "-a") == 0)) {
        int check = psh_no_arguments(session, argc, argv);
        if(check >= 0)
            return check;
    }

    return psh_out(session, "%s\n", psx_platform_uname()) < 0 ? 1 : 0;
}

int
psh_builtin_whoami(psx_session_t *session, int argc, char **argv)
{
    const char *user = psx_env_get(&session->env, "USER");

    int check = psh_no_arguments(session, argc, argv);
    if(check >= 0)
        return check;

    if(!user || !*user) {
        user = psx_platform_user_name();
    }

    char *printable = psh_display_text(user, false, NULL);
    if(!printable)
        return 1;
    int rc = psh_out(session, "%s\n", printable);
    free(printable);
    return rc < 0 ? 1 : 0;
}

int
psh_builtin_ps(psx_session_t *session, int argc, char **argv)
{
    int check = psh_no_arguments(session, argc, argv);
    if(check >= 0)
        return check;
    int status = 0;

    if(psh_out(session, "PSXTerm %s on %s\ndaemon pid: %d\n",
               PSXTERM_VERSION_STRING, psx_platform_name(), (int)getpid()) < 0)
        return 1;

    if(session->manager) {
        int id_width = 2, pid_width = 6;
        for(const psx_session_t *s = session->manager->sessions; s;
            s = s->next) {
            char value[32];
            int n = snprintf(value, sizeof(value), "%u", s->id);
            if(n > id_width)
                id_width = n;
            n = snprintf(value, sizeof(value), "%d", (int)s->proc.pid);
            if(n > pid_width)
                pid_width = n;
        }
        if(psh_out(session, "%*s  %-10s  %*s  %-10s  CLIENT\n", id_width, "ID",
                   "STATE", pid_width, "FG-PID", "TTY") < 0)
            return 1;
        for(const psx_session_t *s = session->manager->sessions; s;
            s = s->next) {
            char *client = psh_display_text(
                s->client_name[0] ? s->client_name : "-", false, NULL);
            if(!client)
                return 1;
            if(psh_out(session, "%*u  %-10s  %*d  %-10s  %s\n", id_width, s->id,
                       psx_session_state_name(s->state), pid_width,
                       (int)s->proc.pid, psx_tty_backend_name(s->tty.backend),
                       client) < 0)
                status = 1;
            free(client);
            if(status)
                return status;
        }
    } else {
        if(psh_out(session, "  ID  STATE       FG-PID  TTY\n") < 0)
            return 1;
        if(psh_out(session, "%4u  %-10s  %6d  %s\n", session->id,
                   psx_session_state_name(session->state),
                   (int)session->proc.pid,
                   psx_tty_backend_name(session->tty.backend)) < 0)
            status = 1;
    }

    return status;
}

int
psh_builtin_clear(psx_session_t *session, int argc, char **argv)
{
    int check = psh_no_arguments(session, argc, argv);
    if(check >= 0)
        return check;

    return psx_session_emit(session, PTTY_MSG_STDOUT, "\x1b[2J\x1b[H", 7) < 0
               ? 1
               : 0;
}
