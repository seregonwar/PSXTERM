#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "psxterm/log.h"

static psx_log_level_t g_level = PSX_LOG_INFO;

void
psx_log_set_level(psx_log_level_t level)
{
    g_level = level;
}

psx_log_level_t
psx_log_get_level(void)
{
    return g_level;
}

void
psx_log(psx_log_level_t level, const char *fmt, ...)
{
    static const char *const names[] = {"error", "warn", "info", "debug"};
    va_list ap;

    if(level > g_level) {
        return;
    }

    fprintf(stderr, "psxterm: %s: ", names[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
