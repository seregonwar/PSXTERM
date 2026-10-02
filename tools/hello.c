/*
 * PSXTerm bring-up probe.
 *
 * Separates raw descriptor writes from libc stdio on a console: the raw
 * descriptor goes straight to the session, while libc stdio may travel
 * through the payload args the loader provides. One run tells which path is
 * connected.
 */

#include <stdio.h>
#include <unistd.h>

int
main(void)
{
    static const char raw_out[] = "HELLO A raw write stdout\n";
    static const char raw_err[] = "HELLO C raw write stderr\n";

    (void)write(STDOUT_FILENO, raw_out, sizeof(raw_out) - 1);
    (void)write(STDERR_FILENO, raw_err, sizeof(raw_err) - 1);

    printf("HELLO B printf stdout\n");
    fflush(stdout);

    fprintf(stderr, "HELLO D fprintf stderr\n");
    fflush(stderr);

    printf("HELLO E printf after fflush\n");
    fflush(stdout);

    return 42;
}
