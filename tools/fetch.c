/*
 * psxterm-fetch: download a URL on the console.
 *
 * Built on the same OpenSSL the rest of the runtime uses, because that part is
 * measured working: name resolution, TCP, a TLS 1.3 handshake, certificate
 * validation against the runtime bundle and a real response all succeed in a
 * payload. curl's own binary does not manage it on this console yet (it exits
 * -1 before printing anything), and a package installer only needs a
 * trustworthy fetch, so this is that piece.
 *
 * Output goes where a shell pipeline expects it: the body on standard output,
 * every message on standard error, so `psxterm-fetch <url> > file` and
 * `psxterm-fetch <url> | sh` both behave.
 *
 * Usage: psxterm-fetch [-o FILE] URL
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#define FETCH_CA "/data/psxterm/runtime/etc/ca-bundle.crt"
#define FETCH_REDIRECTS 5
#define FETCH_MAX_HEADER 16384

static int
fetch_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if(getaddrinfo(host, port, &hints, &result) != 0) {
        fprintf(stderr, "fetch: cannot resolve %s\n", host);
        return -1;
    }

    for(ai = result; ai; ai = ai->ai_next) {
        int candidate = socket(ai->ai_family, ai->ai_socktype,
                               ai->ai_protocol);

        if(candidate < 0) {
            continue;
        }

        if(connect(candidate, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = candidate;
            break;
        }

        close(candidate);
    }

    freeaddrinfo(result);

    if(fd < 0) {
        fprintf(stderr, "fetch: cannot connect to %s:%s\n", host, port);
    }

    return fd;
}

/* Split "https://host/path" into host, port and the request path. */
static int
fetch_split(const char *url, char *host, size_t host_cap, char *port,
            size_t port_cap, char *path, size_t path_cap)
{
    const char *rest;
    const char *slash;
    const char *colon;
    size_t host_len;

    if(strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "fetch: only https:// is supported\n");
        return -1;
    }

    rest = url + 8;
    slash = strchr(rest, '/');
    colon = strchr(rest, ':');
    host_len = slash ? (size_t)(slash - rest) : strlen(rest);

    if(colon && (!slash || colon < slash)) {
        size_t port_len = slash ? (size_t)(slash - colon - 1)
                                : strlen(colon + 1);

        if(port_len + 1 > port_cap || (colon - rest) + 1 > host_cap) {
            return -1;
        }
        snprintf(port, port_cap, "%.*s", (int)port_len, colon + 1);
        host_len = (size_t)(colon - rest);
    } else {
        snprintf(port, port_cap, "443");
    }

    if(host_len + 1 > host_cap) {
        return -1;
    }
    snprintf(host, host_cap, "%.*s", (int)host_len, rest);
    snprintf(path, path_cap, "%s", slash && *slash ? slash : "/");

    return 0;
}

/* Decode a chunked body and write it out. Returns 0 on success. */
static int
fetch_write_chunked(FILE *out, const unsigned char *data, size_t len)
{
    size_t pos = 0;

    while(pos < len) {
        size_t line_end = pos;
        size_t size = 0;
        bool any = false;

        while(line_end < len && data[line_end] != '\n') {
            line_end++;
        }
        if(line_end >= len) {
            break;
        }

        for(size_t i = pos; i < line_end; i++) {
            unsigned char c = data[i];

            if(c == '\r') {
                continue;
            }
            if(c >= '0' && c <= '9') {
                size = size * 16 + (size_t)(c - '0');
                any = true;
            } else if(c >= 'a' && c <= 'f') {
                size = size * 16 + (size_t)(c - 'a' + 10);
                any = true;
            } else if(c >= 'A' && c <= 'F') {
                size = size * 16 + (size_t)(c - 'A' + 10);
                any = true;
            } else {
                break;
            }
        }

        if(!any) {
            break;
        }

        pos = line_end + 1;
        if(size == 0) {
            return 0;
        }
        if(pos + size > len) {
            /* The trailer has not arrived yet; the caller keeps it buffered. */
            return -1;
        }
        if(fwrite(data + pos, 1, size, out) != size) {
            return -1;
        }
        pos += size;

        /* Skip the CRLF that terminates the chunk. */
        if(pos < len && data[pos] == '\r') {
            pos++;
        }
        if(pos < len && data[pos] == '\n') {
            pos++;
        }
    }

    return 0;
}

/* One HTTPS request. Returns the status code, or -1. */
static int
fetch_once(const char *host, const char *port, const char *path, FILE *body,
           char *location, size_t location_cap)
{
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    char request[1024];
    char header[FETCH_MAX_HEADER];
    size_t used = 0;
    int fd = -1;
    int status = -1;
    int eof = 0;
    bool chunked = false;
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    size_t raw_cap = 0;

    if((fd = fetch_connect(host, port)) < 0) {
        return -1;
    }

    if(!(ctx = SSL_CTX_new(TLS_client_method())) ||
       SSL_CTX_load_verify_locations(ctx, FETCH_CA, NULL) != 1) {
        fprintf(stderr, "fetch: cannot set up TLS (%s)\n", FETCH_CA);
        goto out;
    }

    if(!(ssl = SSL_new(ctx))) {
        goto out;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);

    if(SSL_connect(ssl) != 1) {
        fprintf(stderr, "fetch: TLS handshake failed: %s\n",
                ERR_error_string(ERR_get_error(), NULL));
        goto out;
    }

    if(SSL_get_verify_result(ssl) != X509_V_OK) {
        fprintf(stderr, "fetch: certificate not valid for %s (error %ld)\n",
                host, SSL_get_verify_result(ssl));
        goto out;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: psxterm-fetch\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n",
             path, host);

    if(SSL_write(ssl, request, (int)strlen(request)) <= 0) {
        fprintf(stderr, "fetch: request failed\n");
        goto out;
    }

    /* Read the status line and headers, then the body. */
    while(used + 1 < sizeof(header)) {
        int got = SSL_read(ssl, header + used, (int)(sizeof(header) - used - 1));

        if(got <= 0) {
            eof = 1;
            break;
        }
        used += (size_t)got;
        header[used] = '\0';

        {
            char *end = strstr(header, "\r\n\r\n");

            if(end) {
                size_t body_start = (size_t)(end - header) + 4;
                char *line_end = strchr(header, '\r');

                if(sscanf(header, "HTTP/%*d.%*d %d", &status) != 1) {
                    status = -1;
                    goto out;
                }

                chunked = strcasestr(header, "\r\ntransfer-encoding:") &&
                          strcasestr(header, "chunked");

                if(line_end) {
                    *line_end = '\0';
                }
                fprintf(stderr, "fetch: %s\n", header);

                /* A redirect is followed by the caller. */
                {
                    char *loc = strcasestr(header, "\r\nlocation:");

                    if(loc && location) {
                        char *value = loc + 11;

                        while(*value == ' ') {
                            value++;
                        }
                        snprintf(location, location_cap, "%.*s",
                                 (int)strcspn(value, "\r\n"), value);
                    }
                }

                if(body) {
                    size_t rest = used - body_start;

                    if(chunked) {
                        /* Keep the raw chunked stream; an installer must not
                         * receive chunk sizes mixed into its bytes. */
                        if(rest > 0) {
                            raw = malloc(rest);
                            if(!raw) {
                                status = -1;
                                goto out;
                            }
                            memcpy(raw, header + body_start, rest);
                            raw_len = rest;
                            raw_cap = rest;
                        }
                    } else if(rest > 0 &&
                              fwrite(header + body_start, 1, rest, body) !=
                                  rest) {
                        fprintf(stderr, "fetch: cannot write the body\n");
                        status = -1;
                        goto out;
                    }
                }

                break;
            }
        }
    }

    if(!eof && body) {
        for(;;) {
            char block[8192];
            int got = SSL_read(ssl, block, sizeof(block));

            if(got <= 0) {
                break;
            }

            if(!chunked) {
                if(fwrite(block, 1, (size_t)got, body) != (size_t)got) {
                    fprintf(stderr, "fetch: cannot write the body\n");
                    status = -1;
                    goto out;
                }
                continue;
            }

            if(raw_len + (size_t)got > raw_cap) {
                size_t cap = (raw_cap ? raw_cap * 2 : 16384);
                unsigned char *grown;

                while(cap < raw_len + (size_t)got) {
                    cap *= 2;
                }
                if(!(grown = realloc(raw, cap))) {
                    status = -1;
                    goto out;
                }
                raw = grown;
                raw_cap = cap;
            }
            memcpy(raw + raw_len, block, (size_t)got);
            raw_len += (size_t)got;
        }
    }

    if(chunked && raw) {
        if(fetch_write_chunked(body, raw, raw_len) != 0) {
            fprintf(stderr, "fetch: malformed chunked body\n");
            status = -1;
        }
    }

out:
    free(raw);
    if(ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if(ctx) {
        SSL_CTX_free(ctx);
    }
    if(fd >= 0) {
        close(fd);
    }

    return status;
}

int
main(int argc, char **argv)
{
    const char *url = NULL;
    const char *output = NULL;
    FILE *body = stdout;
    char host[256];
    char port[16];
    char path[1024];
    char location[1024];

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else {
            url = argv[i];
        }
    }

    if(!url) {
        fprintf(stderr, "usage: psxterm-fetch [-o FILE] https://url\n");
        return 64;
    }

    if(output) {
        if(!(body = fopen(output, "wb"))) {
            fprintf(stderr, "fetch: cannot write %s: %s\n", output,
                    strerror(errno));
            return 1;
        }
    }

    if(fetch_split(url, host, sizeof(host), port, sizeof(port), path,
                   sizeof(path)) != 0) {
        return 64;
    }

    for(int attempt = 0; attempt <= FETCH_REDIRECTS; attempt++) {
        int status;

        location[0] = '\0';
        status = fetch_once(host, port, path, body, location,
                            sizeof(location));

        if(status >= 300 && status < 400 && location[0]) {
            fprintf(stderr, "fetch: following redirect to %s\n", location);

            if(fetch_split(location, host, sizeof(host), port, sizeof(port),
                           path, sizeof(path)) != 0) {
                break;
            }
            continue;
        }

        if(body != stdout) {
            fclose(body);
        }

        if(status == 200) {
            return 0;
        }

        fprintf(stderr, "fetch: server answered %d\n", status);
        return status > 0 ? 1 : 1;
    }

    fprintf(stderr, "fetch: too many redirects\n");

    return 1;
}
