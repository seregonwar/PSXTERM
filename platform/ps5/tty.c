#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/tty.h"
#include "psxterm/util.h"

/*
 * PS5 FreeBSDPTY backend.
 *
 * The SDK libc ships posix_openpt()/ptsname() declarations but no libutil, so
 * PSXTerm uses the raw kernel interface instead:
 *
 *     open("/dev/ptmx") -> ioctl(TIOCGPTN) -> open("/dev/pts/<n>")
 *
 * Whether /dev/ptmx and /dev/pts are actually usable is a runtime property,
 * not a header property: psx_platform_tty_probe() answers that question and
 * the server falls back to PipeTTY when it fails.
 *
 * HARDWARE TEST REQUIRED: this has not been executed on a PS5.
 */

int
psx_platform_tty_open_pty(psx_tty_t *tty, uint16_t rows, uint16_t cols)
{
    struct winsize ws = {.ws_row = rows, .ws_col = cols};
    char name[32];
    int pts_number = -1;
    int master = -1;
    int slave = -1;

    if((master = open("/dev/ptmx", O_RDWR | O_NOCTTY)) < 0) {
        goto fail;
    }

    if(ioctl(master, TIOCGPTN, &pts_number) < 0) {
        goto fail;
    }

    snprintf(name, sizeof(name), "/dev/pts/%d", pts_number);

    if((slave = open(name, O_RDWR | O_NOCTTY)) < 0) {
        goto fail;
    }

    if(ioctl(slave, TIOCSWINSZ, &ws) < 0) {
        goto fail;
    }

    /* Keep the default termios (canonical, echo, ISIG): external CLI
     * programs expect a normal terminal. */

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

void
psx_platform_tty_probe(psx_tty_probe_result_t *result)
{
    struct winsize ws = {.ws_row = 24, .ws_col = 80};
    struct termios tio;
    struct pollfd pfd;
    int pts_number = -1;
    int master = -1;
    int slave = -1;
    char name[32];
    char byte = 'P';
    char echoed = 0;

    if((master = open("/dev/ptmx", O_RDWR | O_NOCTTY)) < 0) {
        snprintf(result->detail, sizeof(result->detail),
                 "open(/dev/ptmx): %s", strerror(errno));
        return;
    }
    result->ptmx_open = true;

    if(ioctl(master, TIOCGPTN, &pts_number) < 0) {
        snprintf(result->detail, sizeof(result->detail),
                 "TIOCGPTN: %s", strerror(errno));
        goto out;
    }
    result->tiocgptn = true;
    result->pts_number = pts_number;

    snprintf(name, sizeof(name), "/dev/pts/%d", pts_number);

    if((slave = open(name, O_RDWR | O_NOCTTY)) < 0) {
        snprintf(result->detail, sizeof(result->detail), "open(%s): %s", name,
                 strerror(errno));
        goto out;
    }
    result->slave_open = true;

    if(tcgetattr(slave, &tio) == 0) {
        result->termios = true;
    } else {
        snprintf(result->detail, sizeof(result->detail), "tcgetattr: %s",
                 strerror(errno));
    }

    if(ioctl(slave, TIOCGWINSZ, &ws) == 0 && ioctl(slave, TIOCSWINSZ, &ws) == 0) {
        result->winsize = true;
    }

    if(isatty(slave)) {
        result->isatty_true = true;
    }

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
