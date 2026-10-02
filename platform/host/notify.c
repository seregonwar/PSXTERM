/*
 * Host notifications: the same call surface, logged so tests and the client
 * can see what a console would have shown on screen.
 */

#include "psxterm/log.h"
#include "psxterm/platform.h"

void
psx_platform_notify(const char *message)
{
    if(!message || !*message) {
        return;
    }

    PSX_LOGI("notify: %s", message);
}
