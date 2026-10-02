#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "psxterm/tty.h"
#include "psxterm/util.h"

#include "psx_test.h"

static void
test_probe_returns_structured_result(void)
{
    psx_tty_probe_result_t result;

    psx_tty_probe(&result);

    /* The probe must always terminate with a usable fallback decision. */
    PSX_CHECK(result.detail[0] != '\0');
    if(!result.ptmx_open) {
        printf("note: /dev/ptmx unavailable on this host, PipeTTY expected\n");
    }
}

static void
test_default_backend(void)
{
    psx_tty_backend_t backend = psx_tty_default_backend();

    PSX_CHECK(backend == PSX_TTY_BACKEND_FREEBSD_PTY ||
              backend == PSX_TTY_BACKEND_PIPE);
    PSX_CHECK_STR_EQ(psx_tty_backend_name(backend),
                     backend == PSX_TTY_BACKEND_FREEBSD_PTY ? "freebsd-pty"
                                                            : "pipe");
}

static void
test_pipe_backend(void)
{
    psx_tty_t tty;

    psx_tty_init(&tty);
    PSX_CHECK_EQ(tty.master_fd, -1);
    PSX_CHECK_EQ(tty.slave_fd, -1);

    PSX_CHECK_EQ(psx_tty_open(&tty, PSX_TTY_BACKEND_PIPE, 24, 80), 0);
    PSX_CHECK(tty.master_fd >= 0);
    PSX_CHECK(tty.slave_fd >= 0);
    PSX_CHECK(!tty.is_real_pty);
    PSX_CHECK_EQ(tty.backend, PSX_TTY_BACKEND_PIPE);
    PSX_CHECK_EQ(tty.rows, 24);
    PSX_CHECK_EQ(tty.cols, 80);

    PSX_CHECK_EQ(psx_tty_set_size(&tty, 30, 90), 0);
    PSX_CHECK_EQ(tty.rows, 30);
    PSX_CHECK_EQ(tty.cols, 90);

    /* data written on one side is readable on the other */
    PSX_CHECK_EQ(write(tty.master_fd, "ping", 4), 4);
    {
        char buffer[8];
        ssize_t n = read(tty.slave_fd, buffer, sizeof(buffer));

        PSX_CHECK_EQ(n, 4);
        if(n == 4) {
            PSX_CHECK_EQ(memcmp(buffer, "ping", 4), 0);
        }
    }

    psx_tty_close(&tty);
    PSX_CHECK_EQ(tty.master_fd, -1);
    PSX_CHECK_EQ(tty.slave_fd, -1);
    PSX_CHECK_EQ(tty.backend, PSX_TTY_BACKEND_NONE);
}

static void
test_freebsd_pty_backend(void)
{
    psx_tty_t tty;
    psx_tty_probe_result_t probe;

    psx_tty_probe(&probe);
    if(!(probe.ptmx_open && probe.slave_open)) {
        printf("note: skipping FreeBSDPTY test, no PTY available\n");
        return;
    }

    psx_tty_init(&tty);
    PSX_CHECK_EQ(psx_tty_open(&tty, PSX_TTY_BACKEND_FREEBSD_PTY, 24, 80), 0);
    PSX_CHECK(tty.is_real_pty);
    PSX_CHECK_EQ(tty.backend, PSX_TTY_BACKEND_FREEBSD_PTY);
    PSX_CHECK_EQ(isatty(tty.slave_fd), 1);

    PSX_CHECK_EQ(psx_tty_set_size(&tty, 43, 132), 0);

    /* Writing to the slave must produce output on the master (echo). */
    PSX_CHECK_EQ(write(tty.slave_fd, "hello\n", 6), 6);
    {
        struct pollfd pfd = {.fd = tty.master_fd, .events = POLLIN};
        char buffer[64];
        ssize_t n = -1;

        if(poll(&pfd, 1, 500) == 1) {
            n = read(tty.master_fd, buffer, sizeof(buffer));
        }
        PSX_CHECK(n > 0);
        if(n > 0) {
            buffer[n < (ssize_t)sizeof(buffer) ? n : (ssize_t)sizeof(buffer) - 1] = '\0';
            PSX_CHECK(strstr(buffer, "hello") != NULL);
        }
    }

    psx_tty_close(&tty);
}

int
main(void)
{
    test_probe_returns_structured_result();
    test_default_backend();
    test_pipe_backend();
    test_freebsd_pty_backend();

    return PSX_TEST_SUMMARY();
}
