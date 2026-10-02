#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

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
    char message[512];
    char line[600];
    va_list ap;

    if(level > g_level) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    snprintf(line, sizeof(line), "psxterm: %s: %s", names[level], message);

    fputs(line, stderr);
    fputc('\n', stderr);
    fflush(stderr);

    /* Consoles have no visible stderr for a payload: mirror to the platform
     * log sink (kernel log) so bring-up failures remain observable. */
    psx_platform_log_line(line);
}
