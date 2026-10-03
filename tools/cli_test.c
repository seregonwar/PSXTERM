/*
 * cli_test - the controlled external-execution target for PSXTerm.
 *
 * Prints argv, selected environment variables and isatty() results, reads one
 * line from stdin, writes to stdout and stderr, and exits with a known code.
 * Built for the host (integration tests) and for PS4/PS5 (payload ELF).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*
 * Breadcrumbs on the filesystem, written from inside the payload.
 *
 * When the session shows nothing after a certain point, these say whether
 * the payload reached that point at all and whether its own libc stdio can
 * write to the console filesystem - information the session cannot provide
 * by definition, because it is the thing under suspicion.
 */
static void
marker(const char *text)
{
    FILE *file = fopen("/data/psxterm/matrix.log", "a");

    if(file) {
        fprintf(file, "%s\n", text);
        fclose(file);
    }

    {
        int fd = open("/data/psxterm/matrix-raw.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);

        if(fd >= 0) {
            (void)write(fd, text, strlen(text));
            (void)write(fd, "\n", 1);
            close(fd);
        }
    }
}

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

    marker("entry");

    /* Raw markers on the descriptors, so a run that hangs still shows how far
     * it got even when libc stdio is the thing under suspicion. */
    (void)write(STDOUT_FILENO, "CLI-TEST-START\n", 15);

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

    marker("after-stdin");

    /*
     * Stdio acceptance matrix: every call shape a real CLI uses, each with a
     * distinct marker so a missing one names the call that failed. The raw
     * write's result is recorded on the filesystem as well: when the session
     * shows nothing after this point, that value says whether the descriptor
     * itself refused the data inside the payload or whether the data left the
     * process and was lost afterwards.
     */
    {
        ssize_t wrote = write(STDOUT_FILENO, "matrix: write stdout\n", 21);
        int saved = errno;
        char note[128];

        snprintf(note, sizeof(note), "matrix: write(1)=%ld errno=%d",
                 (long)wrote, wrote < 0 ? saved : 0);
        marker(note);
    }

    (void)write(STDERR_FILENO, "matrix: write stderr\n", 21);

    printf("matrix: printf stdout\n");
    fflush(stdout);

    fprintf(stdout, "matrix: fprintf stdout\n");
    fflush(stdout);

    fprintf(stderr, "matrix: fprintf stderr\n");
    fflush(stderr);

    puts("matrix: puts stdout");
    fflush(stdout);

    {
        char block[32];
        size_t got = fread(block, 1, sizeof(block) - 1, stdin);

        block[got] = '\0';
        printf("matrix: fread=%zu\n", got);
        printf("matrix: fread-hex=");
        for(size_t i = 0; i < got; i++) {
            printf("%02x", (unsigned int)(unsigned char)block[i]);
        }
        printf("\n");
        printf("matrix: input eof=%d error=%d\n", feof(stdin) ? 1 : 0,
               ferror(stdin) ? 1 : 0);
        fflush(stdout);
    }

    {
        int stdout_result = fflush(stdout);
        int stderr_result = fflush(stderr);

        printf("matrix: fflush stdout=%d stderr=%d\n", stdout_result,
               stderr_result);
        fflush(stdout);
    }

    marker("end");

    return 7;
}
