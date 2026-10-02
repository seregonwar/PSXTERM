/*
 * Host (Linux/macOS) FreeBSDPTY-compatible backend.
 *
 * Uses the POSIX ptmx API where available; the runtime probe additionally
 * exercises TIOCGPTN and the /dev/pts/<n> path so host results are directly
 * comparable with PS4/PS5 probe results.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "psxterm/tty.h"
#include "psxterm/util.h"

int
psx_platform_tty_open_pty(psx_tty_t *tty, uint16_t rows, uint16_t cols)
{
    struct winsize ws = {.ws_row = rows, .ws_col = cols};
    char *name;
    int master = -1;
    int slave = -1;

    if((master = posix_openpt(O_RDWR | O_NOCTTY)) < 0) {
        goto fail;
    }

    if(grantpt(master) < 0) {
        goto fail;
    }

    if(unlockpt(master) < 0) {
        goto fail;
    }

    if(!(name = ptsname(master))) {
        goto fail;
    }

    if((slave = open(name, O_RDWR | O_NOCTTY)) < 0) {
        goto fail;
    }

    if(ioctl(slave, TIOCSWINSZ, &ws) < 0) {
        goto fail;
    }

    /* Default termios (canonical, echo, ISIG) is what external CLI programs
     * expect from a terminal; do not raw-ify the slave. */

    psx_set_cloexec(master, true);
    psx_set_cloexec(slave, true);

    if(psx_set_nonblocking(master, true) < 0) {
        goto fail;
    }

    tty->master_fd = master;
    tty->slave_fd = slave;
    tty->rows = rows;
    tty->cols = cols;
    tty->is_real_pty = true;
    tty->backend = PSX_TTY_BACKEND_FREEBSD_PTY;

    return 0;

fail:
    {
        int saved = errno;

        if(master >= 0) {
            close(master);
        }
        if(slave >= 0) {
            close(slave);
        }

        errno = saved;
    }

    return -1;
}

int
psx_platform_tty_set_size(psx_tty_t *tty)
{
    struct winsize ws = {.ws_row = tty->rows, .ws_col = tty->cols};

    if(tty->slave_fd >= 0 && ioctl(tty->slave_fd, TIOCSWINSZ, &ws) == 0) {
        return 0;
    }

    if(tty->master_fd >= 0 && ioctl(tty->master_fd, TIOCSWINSZ, &ws) == 0) {
        return 0;
    }

    return -1;
}

static void
probe_fail(psx_tty_probe_result_t *result, const char *what)
{
    snprintf(result->detail, sizeof(result->detail), "%s: %s", what,
             strerror(errno));
}

void
psx_platform_tty_probe(psx_tty_probe_result_t *result)
{
    struct winsize ws = {.ws_row = 24, .ws_col = 80};
    struct termios tio;
    struct pollfd pfd;
    int pts_number = -1;
    int master = -1;
    int slave = -1;
    char name[64];
    char byte = 'P';
    char echoed = 0;

    if((master = posix_openpt(O_RDWR | O_NOCTTY)) < 0) {
        probe_fail(result, "posix_openpt");
        return;
    }
    result->ptmx_open = true;

    if(grantpt(master) < 0 || unlockpt(master) < 0) {
        probe_fail(result, "grantpt/unlockpt");
        goto out;
    }

    if(ioctl(master, TIOCGPTN, &pts_number) == 0) {
        result->tiocgptn = true;
        result->pts_number = pts_number;
    } else {
        snprintf(result->detail, sizeof(result->detail),
                 "TIOCGPTN not supported on this host");
    }

    if(pts_number >= 0) {
        snprintf(name, sizeof(name), "/dev/pts/%d", pts_number);
    } else {
        char *pname = ptsname(master);

        if(!pname) {
            probe_fail(result, "ptsname");
            goto out;
        }
        snprintf(name, sizeof(name), "%s", pname);
    }

    if((slave = open(name, O_RDWR | O_NOCTTY)) < 0) {
        probe_fail(result, "open(slave)");
        goto out;
    }
    result->slave_open = true;

    if(tcgetattr(slave, &tio) == 0 && tcsetattr(slave, TCSANOW, &tio) == 0) {
        result->termios = true;
    } else {
        probe_fail(result, "termios");
    }

    if(ioctl(slave, TIOCGWINSZ, &ws) == 0 && ioctl(slave, TIOCSWINSZ, &ws) == 0) {
        result->winsize = true;
    } else {
        probe_fail(result, "winsize");
    }

    if(isatty(slave)) {
        result->isatty_true = true;
    }

    /* Raw mode so a single byte round-trips without a newline. */
    if(result->termios && tcgetattr(slave, &tio) == 0) {
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(slave, TCSANOW, &tio);
    }

    if(write(master, &byte, 1) == 1) {
        pfd.fd = slave;
        pfd.events = POLLIN;

        if(poll(&pfd, 1, 200) == 1 && read(slave, &echoed, 1) == 1 &&
           echoed == byte) {
            result->io_roundtrip = true;
        }
    }

out:
    if(master >= 0) {
        close(master);
    }
    if(slave >= 0) {
        close(slave);
    }
}
