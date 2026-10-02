#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Generic TTY abstraction.
 *
 * The rest of PSXTerm never cares whether a session is backed by a real
 * kernel PTY or by a pipe/socketpair fallback. Both backends expose the same
 * psx_tty_t handles; only is_real_pty and backend differ.
 *
 * Limitations of the PipeTTY backend (explicitly documented, not hidden):
 *   - no kernel line discipline: no echo, no canonical mode, no line editing
 *   - no TIOCSWINSZ: window size is tracked in userspace only
 *   - no isatty() for spawned processes
 *   - no terminal generated signals: Ctrl+C must be translated by the server
 */

typedef enum {
    PSX_TTY_BACKEND_NONE = 0,
    PSX_TTY_BACKEND_FREEBSD_PTY = 1,
    PSX_TTY_BACKEND_PIPE = 2
} psx_tty_backend_t;

typedef struct {
    int master_fd;
    int slave_fd;

    uint16_t rows;
    uint16_t cols;

    bool is_real_pty;
    psx_tty_backend_t backend;
} psx_tty_t;

/* Structured result of the runtime PTY capability probe. */
typedef struct {
    bool ptmx_open;   /* /dev/ptmx opened */
    bool tiocgptn;    /* TIOCGPTN returned a pts number */
    bool slave_open;  /* /dev/pts/<n> opened */
    bool termios;     /* tcgetattr/tcsetattr succeeded */
    bool winsize;     /* TIOCGWINSZ/TIOCSWINSZ succeeded */
    bool isatty_true; /* isatty(slave) reported true */
    bool io_roundtrip;/* data written to master arrived on the slave */
    int pts_number;
    char detail[192]; /* first failure or extra context */
} psx_tty_probe_result_t;

void psx_tty_init(psx_tty_t *tty);
void psx_tty_close(psx_tty_t *tty);

/* Backend that should be used on this platform right now. */
psx_tty_backend_t psx_tty_default_backend(void);
const char *psx_tty_backend_name(psx_tty_backend_t backend);

/* Force a backend (PSX_TTY_BACKEND_NONE restores runtime auto-detection). */
void psx_tty_force_backend(psx_tty_backend_t backend);

/* Open a tty; returns 0 on success, -1 on error (errno set). */
int psx_tty_open(psx_tty_t *tty, psx_tty_backend_t backend, uint16_t rows,
                 uint16_t cols);

/* Apply a new window size. No-op (but tracked) for PipeTTY. */
int psx_tty_set_size(psx_tty_t *tty, uint16_t rows, uint16_t cols);

/* Runs the platform capability probe (never fatal). */
void psx_tty_probe(psx_tty_probe_result_t *result);
void psx_tty_probe_print(const psx_tty_probe_result_t *result, FILE *out);

/* --- platform hooks, implemented under platform/<plat>/tty.c ------------ */
int psx_platform_tty_open_pty(psx_tty_t *tty, uint16_t rows, uint16_t cols);
int psx_platform_tty_set_size(psx_tty_t *tty);
void psx_platform_tty_probe(psx_tty_probe_result_t *result);
