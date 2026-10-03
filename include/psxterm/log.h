#pragma once

#include <stdbool.h>

typedef enum {
    PSX_LOG_ERROR = 0,
    PSX_LOG_WARN = 1,
    PSX_LOG_INFO = 2,
    PSX_LOG_DEBUG = 3
} psx_log_level_t;

void psx_log_set_level(psx_log_level_t level);
psx_log_level_t psx_log_get_level(void);

/*
 * Whether log lines are also written to the process's standard error.
 *
 * A command runner started through the loader shares its standard output with
 * the user, so it turns this off: the log still reaches the platform sink
 * (kernel log, file mirror) for diagnosis, but the user's view carries only
 * what the command itself produced.
 */
void psx_log_set_stream(bool enabled);
bool psx_log_get_stream(void);

void psx_log(psx_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define PSX_LOGE(...) psx_log(PSX_LOG_ERROR, __VA_ARGS__)
#define PSX_LOGW(...) psx_log(PSX_LOG_WARN, __VA_ARGS__)
#define PSX_LOGI(...) psx_log(PSX_LOG_INFO, __VA_ARGS__)
#define PSX_LOGD(...) psx_log(PSX_LOG_DEBUG, __VA_ARGS__)

/*
 * User-facing notifications: the handful of lifecycle events an operator
 * needs (start, port, instance replacement, sessions, failures, shutdown).
 * Always prefixed, always concise - never per-frame or per-byte noise.
 */
#define PSX_NOTIFY(...) psx_log(PSX_LOG_INFO, "PSXTERM: " __VA_ARGS__)
