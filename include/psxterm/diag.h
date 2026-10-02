#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Structured diagnostics ("PSXTerm Doctor").
 *
 * Results are collected in a machine-readable form; the human-readable and
 * JSON renderings are presentation layers over the same data, so adding
 * --json later never requires rewriting diagnostic logic.
 *
 * Checks must report what they actually observed. A value that cannot be
 * discovered (for example console firmware) is reported as unknown instead of
 * being guessed.
 */

typedef enum {
    PSX_DIAG_PASS = 0,
    PSX_DIAG_FAIL,
    PSX_DIAG_WARN,
    PSX_DIAG_SKIP,
    PSX_DIAG_UNKNOWN
} psx_diag_status_t;

#define PSX_DIAG_NAME_MAX 48
#define PSX_DIAG_DETAIL_MAX 192

typedef struct {
    char name[PSX_DIAG_NAME_MAX];
    psx_diag_status_t status;
    int error_code; /* errno-style code, 0 when not applicable */
    char detail[PSX_DIAG_DETAIL_MAX];
} psx_diag_check_t;

typedef struct {
    char name[PSX_DIAG_NAME_MAX];
    psx_diag_check_t *checks;
    size_t count;
    size_t capacity;
} psx_diag_group_t;

typedef struct {
    psx_diag_group_t *groups;
    size_t count;
    size_t capacity;
    uint64_t elapsed_ms;
} psx_diag_report_t;

/* Exit codes used by `psxterm doctor` and `psxtermd --doctor`. */
enum {
    PSX_DIAG_EXIT_READY = 0,
    PSX_DIAG_EXIT_WARNINGS = 1,
    PSX_DIAG_EXIT_FAILED = 2
};

void psx_diag_report_init(psx_diag_report_t *report);
void psx_diag_report_free(psx_diag_report_t *report);

/* Finds or creates a group. */
psx_diag_group_t *psx_diag_group(psx_diag_report_t *report, const char *name);

void psx_diag_add(psx_diag_group_t *group, const char *name,
                  psx_diag_status_t status, int error_code,
                  const char *detail_fmt, ...)
    __attribute__((format(printf, 5, 6)));

const char *psx_diag_status_name(psx_diag_status_t status);
psx_diag_status_t psx_diag_overall_status(const psx_diag_report_t *report);
const char *psx_diag_overall_name(psx_diag_status_t overall);
int psx_diag_exit_code(psx_diag_status_t overall);

/*
 * Renderings. Both return the number of bytes needed (excluding the
 * terminating NUL); pass out == NULL or a small buffer to learn the size.
 */
size_t psx_diag_format_human(const psx_diag_report_t *report, char *out,
                             size_t out_cap);
size_t psx_diag_format_json(const psx_diag_report_t *report, char *out,
                            size_t out_cap);

struct psx_session;

/*
 * Runs the standard diagnostic suite. `session` may be NULL for local
 * (on-console) operation, in which case session/socket checks are skipped.
 *
 * The suite reuses the runtime TTY probe and executes the controlled
 * cli_test target through the real process backend, so it validates the same
 * code paths a terminal session uses.
 */
void psx_diag_run(psx_diag_report_t *report, const struct psx_session *session);
