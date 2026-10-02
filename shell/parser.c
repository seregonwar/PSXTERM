#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "psxterm/shell.h"

#define PSH_MAX_ARGS 64
#define PSH_MAX_ARG_LEN 1024

static int
token_push(char **argv, int argc, char *token, size_t len)
{
    if(argc >= PSH_MAX_ARGS) {
        errno = E2BIG;
        return -1;
    }

    token[len] = '\0';
    argv[argc] = token;

    return 0;
}

/*
 * Splits a command line into argv.
 *
 * Supported syntax: whitespace-separated words, double quotes, backslash
 * escapes. Adjacent quoted/unquoted segments are concatenated (ab"cd"ef).
 */
int
psh_parse_line(const char *line, int *argc_out, char ***argv_out)
{
    char **argv;
    int argc = 0;
    const char *p = line;

    if(!(argv = calloc(PSH_MAX_ARGS + 1, sizeof(char *)))) {
        return -1;
    }

    while(*p) {
        char *token;
        size_t len = 0;

        while(*p == ' ' || *p == '\t') {
            p++;
        }

        if(!*p) {
            break;
        }

        if(!(token = malloc(PSH_MAX_ARG_LEN + 1))) {
            goto fail;
        }

        while(*p && *p != ' ' && *p != '\t') {
            if(*p == '"') {
                p++;
                while(*p && *p != '"') {
                    char c = *p++;

                    if(c == '\\' && (*p == '"' || *p == '\\')) {
                        c = *p++;
                    }
                    if(len >= PSH_MAX_ARG_LEN) {
                        errno = E2BIG;
                        free(token);
                        goto fail;
                    }
                    token[len++] = c;
                }
                if(*p != '"') {
                    /* unterminated quote */
                    errno = EINVAL;
                    free(token);
                    goto fail;
                }
                p++;
            } else if(*p == '\\' && p[1]) {
                if(len >= PSH_MAX_ARG_LEN) {
                    errno = E2BIG;
                    free(token);
                    goto fail;
                }
                token[len++] = *++p;
                p++;
            } else {
                if(len >= PSH_MAX_ARG_LEN) {
                    errno = E2BIG;
                    free(token);
                    goto fail;
                }
                token[len++] = *p++;
            }
        }

        if(token_push(argv, argc, token, len) < 0) {
            free(token);
            goto fail;
        }

        argc++;
    }

    *argc_out = argc;
    *argv_out = argv;

    return 0;

fail:
    for(int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);

    return -1;
}

void
psh_argv_free(int argc, char **argv)
{
    if(!argv) {
        return;
    }

    for(int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}
