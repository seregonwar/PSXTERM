/*
 * cli_test - the controlled external-execution target for PSXTerm.
 *
 * Prints argv, selected environment variables and isatty() results, reads one
 * line from stdin, writes to stdout and stderr, and exits with a known code.
 * Built for the host (integration tests) and for PS4/PS5 (payload ELF).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static void
print_env(const char *name)
{
    const char *value = getenv(name);

    printf("env %s=%s\n", name, value && *value ? value : "(unset)");
}

int
main(int argc, char **argv)
{
    char line[128];

    /*
     * On a console the standard output is a socket, not a terminal (there is
     * no /dev/ptmx), so libc would fully buffer it and nothing would be
     * visible until the buffer fills or the process exits. This test target
     * makes its own output immediate, exactly like any payload that wants to
     * be seen on a console has to.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("cli_test: argc=%d\n", argc);
    for(int i = 0; i < argc; i++) {
        printf("argv[%d]=%s\n", i, argv[i]);
    }

    print_env("TERM");
    print_env("PATH");
    print_env("PSXTERM");
    print_env("PSXTERM_DOCTOR");
    print_env("USER");

    printf("isatty: stdin=%d stdout=%d stderr=%d\n", isatty(0) ? 1 : 0,
           isatty(1) ? 1 : 0, isatty(2) ? 1 : 0);

    {
        struct winsize ws;

        memset(&ws, 0, sizeof(ws));
        if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
            printf("winsize: rows=%u cols=%u\n", (unsigned)ws.ws_row,
                   (unsigned)ws.ws_col);
        } else {
            printf("winsize: unavailable\n");
        }
    }

    printf("stdin> ");
    fflush(stdout);

    if(fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);

        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        printf("read=%s\n", line);
    } else {
        printf("read=<eof>\n");
    }

    fprintf(stderr, "cli_test: stderr works\n");
    printf("cli_test: exiting with 7\n");

    return 7;
}
