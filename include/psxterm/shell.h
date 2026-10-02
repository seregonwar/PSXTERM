#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "psxterm/session.h"

/*
 * psh - PSXTerm's minimal native shell.
 *
 * The parser intentionally supports only:
 *     command arg1 arg2 "argument with spaces"
 * No globbing, no pipes, no redirection, no variable expansion.
 */

typedef int (*psh_command_fn)(psx_session_t *session, int argc, char **argv);

#define PSH_LINE_MAX 4096

typedef struct {
    const char *name;
    const char *description;
    psh_command_fn execute;
} psh_command_t;

const psh_command_t *psh_registry_lookup(const char *name);
const psh_command_t *psh_registry_table(size_t *count);

/* Splits a line into argv. Returns 0 on success, -1 on parse error.
 * On success the caller owns *argv_out and must release it with
 * psh_argv_free(). */
int psh_parse_line(const char *line, int *argc_out, char ***argv_out);
void psh_argv_free(int argc, char **argv);

/* Builtin output helpers (STDOUT / STDERR frames). */
int psh_out(psx_session_t *session, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int psh_err(psx_session_t *session, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* --- shell instance ----------------------------------------------------- */

typedef struct psx_shell psx_shell_t;

psx_shell_t *psh_shell_create(psx_session_t *session);
void psh_shell_destroy(psx_shell_t *shell);

void psh_shell_prompt(psx_shell_t *shell);

/* Consume raw keyboard bytes from the client. */
void psh_shell_feed(psx_shell_t *shell, const uint8_t *data, size_t len);

bool psh_shell_exit_requested(const psx_shell_t *shell);
int psh_shell_exit_code(const psx_shell_t *shell);
void psh_shell_request_exit(psx_shell_t *shell, int code);

/* Execute a scripted line (used by the EXEC protocol message and tests). */
int psh_shell_execute_line(psx_shell_t *shell, const char *line);

/* --- builtins ----------------------------------------------------------- */
int psh_builtin_help(psx_session_t *session, int argc, char **argv);
int psh_builtin_pwd(psx_session_t *session, int argc, char **argv);
int psh_builtin_cd(psx_session_t *session, int argc, char **argv);
int psh_builtin_ls(psx_session_t *session, int argc, char **argv);
int psh_builtin_cat(psx_session_t *session, int argc, char **argv);
int psh_builtin_clear(psx_session_t *session, int argc, char **argv);
int psh_builtin_env(psx_session_t *session, int argc, char **argv);
int psh_builtin_export(psx_session_t *session, int argc, char **argv);
int psh_builtin_unset(psx_session_t *session, int argc, char **argv);
int psh_builtin_uname(psx_session_t *session, int argc, char **argv);
int psh_builtin_whoami(psx_session_t *session, int argc, char **argv);
int psh_builtin_ps(psx_session_t *session, int argc, char **argv);
int psh_builtin_exit(psx_session_t *session, int argc, char **argv);
