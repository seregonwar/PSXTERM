/*
 * The console clock, and why it matters here.
 *
 * Measured on hardware: the kernel clock reports 2012, so every modern
 * certificate looks either not-yet-valid or expired and TLS verification can
 * never succeed - the TLS probe completes its handshake and then reports the
 * certificate as invalid. Anything that wants a validated HTTPS request (a
 * package installer, an agent CLI) therefore needs the clock to be right
 * first.
 *
 * The correction comes from the network the console already has: one SNTP
 * request, and the clock is set only when it is clearly wrong, so a console
 * with a good clock is never touched.
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/util.h"

#define CLOCK_SANE_YEAR 2020
#define CLOCK_SERVER "pool.ntp.org"
#define CLOCK_PORT "123"
#define CLOCK_NTP_EPOCH_DELTA 2208988800ul
#define CLOCK_WAIT_MS 4000

static bool
clock_looks_sane(void)
{
    time_t now = time(NULL);
    struct tm tm_utc;

    if(!gmtime_r(&now, &tm_utc)) {
        return false;
    }

    return (tm_utc.tm_year + 1900) >= CLOCK_SANE_YEAR;
}

/* Ask one server for the time. Returns the seconds since the Unix epoch, or 0. */
static uint32_t
clock_query(const char *host)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    uint8_t request[48];
    uint8_t reply[48];
    uint32_t seconds = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    if(getaddrinfo(host, CLOCK_PORT, &hints, &result) != 0) {
        return 0;
    }

    for(struct addrinfo *ai = result; ai && !seconds; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        struct timeval timeout = {.tv_sec = CLOCK_WAIT_MS / 1000,
                                  .tv_usec = (CLOCK_WAIT_MS % 1000) * 1000};

        if(fd < 0) {
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        /* SNTP version 3, client mode. */
        memset(request, 0, sizeof(request));
        request[0] = 0x1b;

        if(sendto(fd, request, sizeof(request), 0, ai->ai_addr,
                  ai->ai_addrlen) == (ssize_t)sizeof(request)) {
            ssize_t got = recv(fd, reply, sizeof(reply), 0);

            if(got >= 44) {
                uint32_t transmit = ((uint32_t)reply[40] << 24) |
                                    ((uint32_t)reply[41] << 16) |
                                    ((uint32_t)reply[42] << 8) |
                                    (uint32_t)reply[43];

                if(transmit > CLOCK_NTP_EPOCH_DELTA) {
                    seconds = transmit - CLOCK_NTP_EPOCH_DELTA;
                }
            }
        }

        close(fd);
    }

    freeaddrinfo(result);

    return seconds;
}

bool
psx_clock_sync_if_needed(void)
{
    uint32_t now;

    if(clock_looks_sane()) {
        return true;
    }

    if((now = clock_query(CLOCK_SERVER)) == 0) {
        PSX_LOGW("clock: %s did not answer; certificate validation will fail "
                 "until the console clock is right",
                 CLOCK_SERVER);
        return false;
    }

    {
        struct timespec ts = {.tv_sec = (time_t)now, .tv_nsec = 0};

        /* clock_settime rather than settimeofday: plain POSIX, so the same
         * code builds for the host and for the console. */
        if(clock_settime(CLOCK_REALTIME, &ts) != 0) {
            PSX_LOGW("clock: cannot set the time: %s", strerror(errno));
            return false;
        }
    }

    PSX_NOTIFY("clock corrected from %s", CLOCK_SERVER);

    return true;
}
