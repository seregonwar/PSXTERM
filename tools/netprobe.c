/*
 * psxterm-netprobe: can a console payload reach the network?
 *
 * curl starting but doing nothing (no output, no file, no error) is
 * indistinguishable from a network that a payload cannot use, so this probe
 * answers that question first and in one line: resolve a name, connect, and
 * report each step through the standard io it was given.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int
probe(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char text[256];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, port, &hints, &result);
    if(rc != 0) {
        printf("netprobe: DNS %s failed: %s\n", host, gai_strerror(rc));
        return 1;
    }

    for(ai = result; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if(fd < 0) {
            continue;
        }

        if(ai->ai_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;

            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
        } else {
            snprintf(text, sizeof(text), "(family %d)", ai->ai_family);
        }

        printf("netprobe: %s resolves to %s\n", host, text);
        fflush(stdout);

        if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            printf("netprobe: connected to %s:%s\n", host, port);
            fflush(stdout);
            close(fd);
            freeaddrinfo(result);
            return 0;
        }

        printf("netprobe: connect to %s:%s failed: %s\n", host, port,
               strerror(errno));
        fflush(stdout);
        close(fd);
    }

    freeaddrinfo(result);

    return 1;
}

int
main(void)
{
    int failures = 0;

    fflush(stdout);
    printf("netprobe: start\n");
    fflush(stdout);

    failures += probe("example.com", "443");
    failures += probe("1.1.1.1", "443");

    printf("netprobe: done (%d failures)\n", failures);
    fflush(stdout);

    return failures == 0 ? 0 : 1;
}
