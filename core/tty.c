#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/tty.h"
#include "psxterm/util.h"

void
psx_tty_init(psx_tty_t *tty)
{
    memset(tty, 0, sizeof(*tty));
    tty->master_fd = -1;
    tty->slave_fd = -1;
    tty->backend = PSX_TTY_BACKEND_NONE;
}

void
psx_tty_close(psx_tty_t *tty)
{
    if(tty->master_fd >= 0) {
        close(tty->master_fd);
    }
    if(tty->slave_fd >= 0) {
        close(tty->slave_fd);
    }

    psx_tty_init(tty);
}

const char *
psx_tty_backend_name(psx_tty_backend_t backend)
{
    switch(backend) {
    case PSX_TTY_BACKEND_FREEBSD_PTY: return "freebsd-pty";
    case PSX_TTY_BACKEND_PIPE: return "pipe";
    case PSX_TTY_BACKEND_NONE: return "none";
    default: return "unknown";
    }
}

static int g_forced_backend = -1;

void
psx_tty_force_backend(psx_tty_backend_t backend)
{
    g_forced_backend =
        backend == PSX_TTY_BACKEND_NONE ? -1 : (int)backend;
}

psx_tty_backend_t
psx_tty_default_backend(void)
{
    static int cached = -1;

    if(g_forced_backend >= 0) {
        return (psx_tty_backend_t)g_forced_backend;
    }

    if(cached < 0) {
        psx_tty_probe_result_t probe;

        psx_tty_probe(&probe);
        cached = (probe.ptmx_open && probe.tiocgptn && probe.slave_open &&
                  probe.termios && probe.winsize)
                     ? PSX_TTY_BACKEND_FREEBSD_PTY
                     : PSX_TTY_BACKEND_PIPE;

        if(cached == PSX_TTY_BACKEND_FREEBSD_PTY) {
            PSX_LOGI("tty: FreeBSDPTY backend available (pts %d)",
                     probe.pts_number);
        } else {
            PSX_LOGW("tty: real PTY not available, using PipeTTY fallback "
                     "(%s)",
                     probe.detail[0] ? probe.detail : "probe failed");
        }
    }

    return (psx_tty_backend_t)cached;
}

static int
tty_open_pipe(psx_tty_t *tty, uint16_t rows, uint16_t cols)
{
    int fds[2];

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        return -1;
    }

    psx_set_cloexec(fds[0], true);
    psx_set_cloexec(fds[1], true);

    /* Only the daemon side is non-blocking; the child side must stay
     * blocking. */
    if(psx_set_nonblocking(fds[0], true) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    tty->master_fd = fds[0];
    tty->slave_fd = fds[1];
    tty->rows = rows;
    tty->cols = cols;
    tty->is_real_pty = false;
    tty->backend = PSX_TTY_BACKEND_PIPE;

    return 0;
}

int
psx_tty_open(psx_tty_t *tty, psx_tty_backend_t backend, uint16_t rows,
             uint16_t cols)
{
    psx_tty_init(tty);

    switch(backend) {
    case PSX_TTY_BACKEND_FREEBSD_PTY:
        return psx_platform_tty_open_pty(tty, rows, cols);

    case PSX_TTY_BACKEND_PIPE:
        return tty_open_pipe(tty, rows, cols);

    default:
        errno = EINVAL;
        return -1;
    }
}

int
psx_tty_set_size(psx_tty_t *tty, uint16_t rows, uint16_t cols)
{
    tty->rows = rows;
    tty->cols = cols;

    if(tty->backend == PSX_TTY_BACKEND_FREEBSD_PTY) {
        return psx_platform_tty_set_size(tty);
    }

    /* PipeTTY tracks the size in userspace only; there is no terminal to
     * inform. */
    return 0;
}

void
psx_tty_probe(psx_tty_probe_result_t *result)
{
    memset(result, 0, sizeof(*result));

    psx_platform_tty_probe(result);

    if(!result->detail[0]) {
        snprintf(result->detail, sizeof(result->detail),
                 result->ptmx_open ? "probe completed"
                                   : "real PTY not available");
    }
}

void
psx_tty_probe_print(const psx_tty_probe_result_t *result, FILE *out)
{
    fprintf(out, "PSXTerm TTY capability probe\n");
    fprintf(out, "  /dev/ptmx open .......... %s\n",
            result->ptmx_open ? "yes" : "no");
    fprintf(out, "  TIOCGPTN ................ %s", result->tiocgptn ? "yes" : "no");
    if(result->tiocgptn) {
        fprintf(out, " (pts %d)", result->pts_number);
    }
    fputc('\n', out);
    fprintf(out, "  /dev/pts/<n> open ....... %s\n",
            result->slave_open ? "yes" : "no");
    fprintf(out, "  termios tcgetattr ....... %s\n",
            result->termios ? "yes" : "no");
    fprintf(out, "  window size ioctl ....... %s\n",
            result->winsize ? "yes" : "no");
    fprintf(out, "  isatty(slave) ........... %s\n",
            result->isatty_true ? "yes" : "no");
    fprintf(out, "  master/slave roundtrip .. %s\n",
            result->io_roundtrip ? "yes" : "no");
    fprintf(out, "  detail: %s\n",
            result->detail[0] ? result->detail : "(none)");
    fprintf(out, "  FreeBSDPTY backend ...... %s\n",
            (result->ptmx_open && result->tiocgptn && result->slave_open &&
             result->termios && result->winsize)
                ? "available"
                : "not available");
    fprintf(out, "  PipeTTY fallback ........ available\n");
}
