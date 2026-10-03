/*
 * psxterm-fetchprobe: is libpsl usable inside a payload?
 *
 * Every payload runs through the same injection path, and tlsprobe (OpenSSL
 * and zlib, no libpsl) works while curl - the only thing in the bundle that
 * links libpsl - starts and then does nothing. This probe links the same
 * set curl does and touches libpsl's API, so its behaviour says whether that
 * library is where curl stops.
 */

#include <stdio.h>
#include <stdlib.h>

#include <libpsl.h>

int
main(void)
{
    char *lower = NULL;
    psl_error_t rc;
    const char *domain;
    int failures = 0;

    printf("fetchprobe: start\n");
    fflush(stdout);

    rc = psl_str_to_utf8lower("EXAMPLE.com", "utf-8", NULL, &lower);
    printf("fetchprobe: psl_str_to_utf8lower rc=%d out=%s\n", (int)rc,
           lower ? lower : "(null)");
    fflush(stdout);
    psl_free(lower);
    lower = NULL;

    domain = psl_registrable_domain(psl_builtin, "www.example.com");
    printf("fetchprobe: registrable domain: %s\n",
           domain ? domain : "(none)");
    fflush(stdout);

    if(domain == NULL) {
        failures++;
    }

    printf("fetchprobe: done (%d failures)\n", failures);
    fflush(stdout);

    return failures == 0 ? 0 : 1;
}
