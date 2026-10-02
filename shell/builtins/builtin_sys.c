#include <unistd.h>

#include "psxterm/platform.h"
#include "psxterm/shell.h"
#include "psxterm/version.h"

int
psh_builtin_uname(psx_session_t *session, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return psh_out(session, "%s\n", psx_platform_uname()) < 0 ? 1 : 0;
}

int
psh_builtin_whoami(psx_session_t *session, int argc, char **argv)
{
    const char *user = psx_env_get(&session->env, "USER");

    (void)argc;
    (void)argv;

    if(!user || !*user) {
        user = psx_platform_user_name();
    }

    return psh_out(session, "%s\n", user) < 0 ? 1 : 0;
}

int
psh_builtin_ps(psx_session_t *session, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    psh_out(session, "PSXTerm %s on %s\n", PSXTERM_VERSION_STRING,
            psx_platform_name());
    psh_out(session, "daemon pid: %d\n", (int)getpid());

    if(session->manager) {
        psh_out(session, "  ID  STATE       FG-PID  TTY         CLIENT\n");
        for(const psx_session_t *s = session->manager->sessions; s; s = s->next) {
            psh_out(session, "%4u  %-10s  %6d  %-10s  %s\n", s->id,
                    psx_session_state_name(s->state), (int)s->proc.pid,
                    psx_tty_backend_name(s->tty.backend),
                    s->client_name[0] ? s->client_name : "-");
        }
    } else {
        psh_out(session, "  ID  STATE       FG-PID  TTY\n");
        psh_out(session, "%4u  %-10s  %6d  %s\n", session->id,
                psx_session_state_name(session->state), (int)session->proc.pid,
                psx_tty_backend_name(session->tty.backend));
    }

    return 0;
}

int
psh_builtin_clear(psx_session_t *session, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return psx_session_emit(session, PTTY_MSG_STDOUT, "\x1b[2J\x1b[H", 7) < 0
               ? 1
               : 0;
}
