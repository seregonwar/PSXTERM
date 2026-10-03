/*
 * Client for the console's payload loader (elfldr, port 9021 on loopback).
 *
 * Measured on hardware: a payload the loader starts receives the loader's
 * connection as its standard io, and in that configuration a normal CLI's
 * output - descriptor writes and libc printf alike - reaches the parent. The
 * daemon's own spawn path cannot reproduce that, so a command that needs real
 * standard io is run this way instead.
 *
 * The command itself is not what travels: the loader receives the wrapper
 * payload (tools/exec_wrapper.c), which then loads the command, hands it the
 * same standard io, and reports the exit status. That keeps this path free of
 * any size limit and free of the argument problem - the wrapper reads the
 * command line from the connection.
 *
 * Reference: zftpd, src/http/games/ps5_install_helper.c, which launches its
 * helper the same way.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/runtime.h"
#include "psxterm/util.h"

#define PSX_LOADER_PORT 9021
#define PSX_LOADER_WRAPPER "/data/psxterm/bin/exec_wrapper.elf"
#define PSX_LOADER_READY_MS 500

static int
loader_connect(void)
{
    struct sockaddr_in addr;
    int fd;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PSX_LOADER_PORT);

    if(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return -1;
    }

    if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
loader_send_file(int fd, const char *path)
{
    uint8_t buffer[16384];
    int file;

    if((file = open(path, O_RDONLY)) < 0) {
        PSX_LOGW("loader: cannot open the wrapper %s: %s", path,
                 strerror(errno));
        return -1;
    }

    for(;;) {
        ssize_t got = read(file, buffer, sizeof(buffer));
        size_t sent = 0;

        if(got < 0) {
            if(errno == EINTR) {
                continue;
            }
            close(file);
            return -1;
        }
        if(got == 0) {
            break;
        }

        while(sent < (size_t)got) {
            ssize_t put = send(fd, buffer + sent, (size_t)got - sent, 0);

            if(put < 0) {
                if(errno == EINTR) {
                    continue;
                }
                /*
                 * The socket is non-blocking: wait for room instead of
                 * blocking the daemon on a loader that stopped reading. A
                 * timed-out descriptor fails here rather than freezing the
                 * whole server, which is how the daemon hung once.
                 */
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    fd_set writefds;
                    struct timeval wait = {.tv_sec = 5, .tv_usec = 0};

                    FD_ZERO(&writefds);
                    FD_SET(fd, &writefds);

                    if(select(fd + 1, NULL, &writefds, NULL, &wait) > 0) {
                        continue;
                    }
                }

                close(file);
                return -1;
            }
            sent += (size_t)put;
        }
    }

    close(file);

    return 0;
}

/* Send a whole buffer on a non-blocking socket, with a bound. */
static int
loader_send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while(sent < len) {
        ssize_t put = send(fd, data + sent, len - sent, 0);

        if(put < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                fd_set writefds;
                struct timeval wait = {.tv_sec = 5, .tv_usec = 0};

                FD_ZERO(&writefds);
                FD_SET(fd, &writefds);

                if(select(fd + 1, NULL, &writefds, NULL, &wait) > 0) {
                    continue;
                }
            }
            return -1;
        }
        sent += (size_t)put;
    }

    return 0;
}

int
psx_platform_loader_exec(const char *command_line)
{
    int fd;
    size_t len;

    if(!command_line || !*command_line) {
        errno = EINVAL;
        return -1;
    }

    if((fd = loader_connect()) < 0) {
        PSX_LOGD("loader: not reachable on port %d", PSX_LOADER_PORT);
        return -1;
    }

    /*
     * Non-blocking from here on, with a send/receive bound as well: the
     * session reads this descriptor expecting EAGAIN, and a blocking one
     * froze the whole daemon when the loader went quiet.
     */
    psx_set_nonblocking(fd, true);
    {
        struct timeval bound = {.tv_sec = 5, .tv_usec = 0};

        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &bound, sizeof(bound));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
    }

    if(loader_send_file(fd, PSX_LOADER_WRAPPER) != 0) {
        close(fd);
        return -1;
    }

    /*
     * The loader needs a moment to start the wrapper before its standard io
     * exists; writing earlier would still be buffered, but waiting keeps the
     * command line from being read as part of the ELF stream.
     */
    {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};

        nanosleep(&pause, NULL);
    }

    len = strlen(command_line);

    if(loader_send_all(fd, command_line, len) != 0 ||
       loader_send_all(fd, "\n", 1) != 0) {
        PSX_LOGW("loader: cannot send the command line");
        close(fd);
        return -1;
    }

    PSX_LOGI("loader: running \"%s\" through the loader", command_line);
    (void)PSX_LOADER_READY_MS;

    return fd;
}
