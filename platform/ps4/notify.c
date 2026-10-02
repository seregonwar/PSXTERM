/*
 * Notifications on the PS4 use the legacy kernel request: the richer
 * interactive toast API is PS5-only. Weak symbol, so a build that links
 * without it simply reports notifications as unavailable.
 *
 * Reference: MemDBG src/pal/pal_notification.c (PS4 branch).
 */

#include <stdio.h>
#include <string.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

typedef struct {
    char padding[45];
    char message[3075];
} psx_notify_request_t;

__attribute__((weak)) int
sceKernelSendNotificationRequest(int, psx_notify_request_t *, size_t, int);

static int g_notify_available = -1;

void
psx_platform_notify(const char *message)
{
    psx_notify_request_t req;

    if(!message || !*message) {
        return;
    }

    if(g_notify_available < 0) {
        g_notify_available = sceKernelSendNotificationRequest != NULL ? 1 : 0;
        if(!g_notify_available) {
            PSX_LOGW("notifications unavailable on this system");
        }
    }

    if(!g_notify_available) {
        return;
    }

    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", message);
    (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    PSX_LOGD("notify (legacy): %s", message);
}
