#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/shell.h"

#define PSH_MSG_MAX 1024

struct psx_shell {
    psx_session_t *session;

    char line[PSH_LINE_MAX];
    size_t line_len;

    bool exit_requested;
    int exit_code;
    int last_status;

    int escape_state; /* 0 none, 1 saw ESC, 2 inside CSI */
    bool last_was_cr;
};

psx_shell_t *
psh_shell_create(psx_session_t *session)
{
    psx_shell_t *shell = calloc(1, sizeof(*shell));

    if(!shell) {
        return NULL;
    }

    shell->session = session;

    return shell;
}

void
psh_shell_destroy(psx_shell_t *shell)
{
    free(shell);
}

static int
shell_vprint(psx_session_t *session, uint8_t type, const char *fmt, va_list ap)
{
    char message[PSH_MSG_MAX];
    char *text = message;
    va_list copy;
    int len;
    int result;

    va_copy(copy, ap);
    len = vsnprintf(message, sizeof(message), fmt, copy);
    va_end(copy);

    if(len < 0) {
        return -1;
    }
    if((size_t)len >= sizeof(message)) {
        if(!(text = malloc((size_t)len + 1))) {
            return -1;
        }
        va_copy(copy, ap);
        vsnprintf(text, (size_t)len + 1, fmt, copy);
        va_end(copy);
    }

    result = psx_session_emit(session, type, text, (size_t)len);
    if(text != message) {
        free(text);
    }
    return result;
}

int
psh_out(psx_session_t *session, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = shell_vprint(session, PTTY_MSG_STDOUT, fmt, ap);
    va_end(ap);
    return result;
}

int
psh_err(psx_session_t *session, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = shell_vprint(session, PTTY_MSG_STDERR, fmt, ap);
    va_end(ap);
    return result;
}

/* Prompt: <platform>:<cwd with $HOME shortened> $ */
void
psh_shell_prompt(psx_shell_t *shell)
{
    psx_session_t *session = shell->session;
    const char *home = psx_env_get(&session->env, "HOME");
    char display[PSX_PATH_MAX];

    if(home && *home) {
        size_t home_len = strlen(home);

        if(strncmp(session->cwd, home, home_len) == 0 &&
           (session->cwd[home_len] == '\0' || session->cwd[home_len] == '/')) {
            snprintf(display, sizeof(display), "~%s", session->cwd + home_len);
        } else {
            snprintf(display, sizeof(display), "%s", session->cwd);
        }
    } else {
        snprintf(display, sizeof(display), "%s", session->cwd);
    }

    char *printable = psh_display_text(display, false, NULL);
    psh_out(shell->session, "%s:%s $ ", psx_platform_id(),
            printable ? printable : "?");
    free(printable);
}

bool
psh_shell_exit_requested(const psx_shell_t *shell)
{
    return shell->exit_requested;
}

int
psh_shell_exit_code(const psx_shell_t *shell)
{
    return shell->exit_code;
}

void
psh_shell_request_exit(psx_shell_t *shell, int code)
{
    shell->exit_requested = true;
    shell->exit_code = code;
}

int
psh_shell_execute_line(psx_shell_t *shell, const char *line)
{
    psx_session_t *session = shell->session;
    const psh_command_t *command;
    char **argv = NULL;
    int argc = 0;
    int status = 0;

    if(psh_parse_line(line, &argc, &argv) < 0) {
        psh_err(session, "psh: parse error\n");
        shell->last_status = 2;
        return 2;
    }

    if(argc == 0) {
        psh_argv_free(argc, argv);
        return shell->last_status;
    }

    if((command = psh_registry_lookup(argv[0]))) {
        status = command->execute(session, argc, argv);
    } else {
        char *path = psx_session_resolve_path(session, argv[0]);

        if(!path) {
            psh_err(session, "psh: %s: command not found\n", argv[0]);
            status = 127;
        } else if(!psx_process_backend_available()) {
            psh_err(session,
                    "psh: %s: external execution is not available on this "
                    "build\n",
                    argv[0]);
            status = 126;
        } else if(psx_session_spawn_process(session, path, argv) < 0) {
            psh_err(session, "psh: %s: %s\n", argv[0], strerror(errno));
            status = 126;
        } else {
            /* The EXIT frame will report the real exit status. */
            status = 0;
        }

        free(path);
    }

    shell->last_status = status;
    psh_argv_free(argc, argv);

    return status;
}

static void
shell_run_line(psx_shell_t *shell)
{
    shell->line[shell->line_len] = '\0';
    psh_shell_execute_line(shell, shell->line);
    shell->line_len = 0;
}

static void
shell_handle_byte(psx_shell_t *shell, uint8_t byte)
{
    psx_session_t *session = shell->session;

    /* Consume ANSI escape sequences (arrow keys etc.) without interpreting. */
    if(shell->escape_state == 1) {
        shell->escape_state = (byte == '[' || byte == 'O') ? 2 : 0;
        return;
    }
    if(shell->escape_state == 2) {
        if(byte >= 0x40 && byte <= 0x7e) {
            shell->escape_state = 0;
        }
        return;
    }

    switch(byte) {
    case 0x1b:
        shell->escape_state = 1;
        shell->last_was_cr = false;
        return;

    case '\r':
    case '\n':
        if(byte == '\n' && shell->last_was_cr) {
            shell->last_was_cr = false;
            return;
        }
        shell->last_was_cr = (byte == '\r');
        psx_session_emit(session, PTTY_MSG_STDOUT, "\r\n", 2);
        shell_run_line(shell);
        if(!shell->exit_requested && !psx_session_has_process(session)) {
            psh_shell_prompt(shell);
        }
        return;

    case 0x7f:
    case 0x08:
        if(shell->line_len > 0) {
            shell->line_len--;
            psx_session_emit(session, PTTY_MSG_STDOUT, "\b \b", 3);
        }
        shell->last_was_cr = false;
        return;

    case 0x03: /* Ctrl+C */
        psx_session_emit(session, PTTY_MSG_STDOUT, "^C\r\n", 4);
        shell->line_len = 0;
        shell->last_status = 130;
        shell->last_was_cr = false;
        psh_shell_prompt(shell);
        return;

    case 0x04: /* Ctrl+D */
        if(shell->line_len == 0) {
            psh_out(session, "exit\n");
            shell->exit_requested = true;
            shell->exit_code = shell->last_status;
        }
        shell->last_was_cr = false;
        return;

    case 0x1a: /* Ctrl+Z: no job control in psh */
        shell->last_was_cr = false;
        return;

    case '\t':
        shell->last_was_cr = false;
        return;

    default:
        break;
    }

    shell->last_was_cr = false;

    if(byte < 0x20) {
        return;
    }

    if(shell->line_len + 1 >= sizeof(shell->line)) {
        psx_session_emit(session, PTTY_MSG_STDOUT, "\a", 1);
        return;
    }

    shell->line[shell->line_len++] = (char)byte;
    psx_session_emit(session, PTTY_MSG_STDOUT, &byte, 1);
}

void
psh_shell_feed(psx_shell_t *shell, const uint8_t *data, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        if(shell->exit_requested) {
            return;
        }
        shell_handle_byte(shell, data[i]);
    }
}
