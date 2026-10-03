/*
 * psxterm-tlsprobe: does TLS work inside a console payload?
 *
 * curl starts and then does nothing at all, which is indistinguishable from
 * TLS being unusable here. This probe uses the same OpenSSL the curl bundle
 * was linked against and reports each step of one real HTTPS request: name
 * resolution, TCP, the TLS handshake, certificate validation against the
 * runtime bundle, one request and one response line.
 *
 * It is deliberately small and built with the payload SDK, so it says nothing
 * about curl's own startup - only about the ground it stands on.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#define PROBE_HOST "example.com"
#define PROBE_PORT "443"
#define PROBE_CA "/data/psxterm/runtime/etc/ca-bundle.crt"
#define PROBE_REQUEST "GET / HTTP/1.0\r\nHost: " PROBE_HOST "\r\n\r\n"

static int
tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char text[64];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if((rc = getaddrinfo(host, port, &hints, &result)) != 0) {
        printf("tlsprobe: DNS failed: %s\n", gai_strerror(rc));
        return -1;
    }

    for(ai = result; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if(fd < 0) {
            continue;
        }

        if(ai->ai_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;

            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
            printf("tlsprobe: %s -> %s\n", host, text);
        }

        if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            freeaddrinfo(result);
            return fd;
        }

        printf("tlsprobe: connect failed: %s\n", strerror(errno));
        close(fd);
    }

    freeaddrinfo(result);

    return -1;
}

int
main(void)
{
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
    char reply[512];
    ssize_t got;

    printf("tlsprobe: OpenSSL %s\n", OpenSSL_version(OPENSSL_VERSION));
    fflush(stdout);

    if((fd = tcp_connect(PROBE_HOST, PROBE_PORT)) < 0) {
        return 1;
    }
    printf("tlsprobe: TCP connected\n");
    fflush(stdout);

    if(!(ctx = SSL_CTX_new(TLS_client_method()))) {
        printf("tlsprobe: no TLS context\n");
        return 1;
    }

    if(SSL_CTX_load_verify_locations(ctx, PROBE_CA, NULL) != 1) {
        printf("tlsprobe: cannot load the CA bundle %s\n", PROBE_CA);
        return 1;
    }
    printf("tlsprobe: CA bundle loaded\n");
    fflush(stdout);

    if(!(ssl = SSL_new(ctx))) {
        printf("tlsprobe: no TLS handle\n");
        return 1;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, PROBE_HOST);

    if(SSL_connect(ssl) != 1) {
        printf("tlsprobe: handshake failed: %s\n",
               ERR_error_string(ERR_get_error(), NULL));
        return 1;
    }

    printf("tlsprobe: handshake ok, %s, %s\n", SSL_get_version(ssl),
           SSL_get_cipher(ssl));

    if(SSL_get_verify_result(ssl) == X509_V_OK) {
        printf("tlsprobe: certificate validated\n");
    } else {
        printf("tlsprobe: certificate NOT valid\n");
    }
    fflush(stdout);

    if(SSL_write(ssl, PROBE_REQUEST, (int)strlen(PROBE_REQUEST)) <= 0) {
        printf("tlsprobe: request failed\n");
        return 1;
    }

    got = SSL_read(ssl, reply, sizeof(reply) - 1);
    if(got <= 0) {
        printf("tlsprobe: no response\n");
        return 1;
    }

    reply[got] = '\0';
    {
        char *nl = strchr(reply, '\r');

        if(nl) {
            *nl = '\0';
        }
    }
    printf("tlsprobe: first line: %s\n", reply);
    fflush(stdout);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    printf("tlsprobe: done\n");

    return 0;
}
