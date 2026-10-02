/*
 * PS5 system notifications.
 *
 * A toast on the console, not a log line: the rich notification API
 * (`sceNotificationSend`) shows the message with the PSXTerm identity, and
 * the legacy request is kept as the fallback for systems where the richer
 * API is unavailable. Both symbols are weak, so a payload that links without
 * them simply reports that notifications are unavailable instead of failing.
 *
 * Ported from the reference PAL notification module (MemDBG,
 * src/pal/pal_notification.c), rebranded for PSXTerm.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"

typedef struct {
    char padding[45];
    char message[3075];
} psx_notify_request_t;

__attribute__((weak)) int
sceKernelSendNotificationRequest(int, psx_notify_request_t *, size_t, int);

__attribute__((weak)) int
sceNotificationSend(int userId, int isLogged, const char *payload);

static int g_notify_available = -1;

static void
notify_json_escape(char *dst, size_t cap, const char *src)
{
    size_t used = strlen(dst);

    for(; *src != '\0' && used + 1 < cap; src++) {
        const char *esc = NULL;
        char one[2] = {0, 0};

        switch(*src) {
        case '\\': esc = "\\\\"; break;
        case '"': esc = "\\\""; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            one[0] = *src;
            esc = one;
            break;
        }

        if(used + strlen(esc) >= cap) {
            break;
        }

        memcpy(dst + used, esc, strlen(esc));
        used += strlen(esc);
        dst[used] = '\0';
    }
}

static int
notify_send_rich(const char *message)
{
    char escaped[3072];
    char payload[6144];
    char created_at[32];
    char notification_id[32];
    time_t now;
    struct tm tm_utc;
    int len;

    if(!sceNotificationSend || !message || !*message) {
        return -1;
    }

    escaped[0] = '\0';
    notify_json_escape(escaped, sizeof(escaped), message);

    now = time(NULL);
    if(!gmtime_r(&now, &tm_utc)) {
        return -1;
    }
    if(strftime(created_at, sizeof(created_at), "%Y-%m-%dT%H:%M:%S.000Z",
                &tm_utc) == 0) {
        return -1;
    }

    snprintf(notification_id, sizeof(notification_id), "%u",
             (unsigned)((uint32_t)now ^ (uint32_t)getpid()));

    len = snprintf(
        payload, sizeof(payload),
        "{"
        "\"rawData\":{"
        "\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"ServiceFeedback\","
        "\"bundleName\":\"PSXTerm\","
        "\"useCaseId\":\"IDC\","
        "\"soundEffect\":\"none\","
        "\"toastOverwriteType\":\"InQueue\","
        "\"isImmediate\":true,"
        "\"priority\":100,"
        "\"viewData\":{"
        "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":"
        "\"notice_info\"}},"
        "\"message\":{\"body\":\"%s\"},"
        "\"subMessage\":{\"body\":\"PSXTerm\"}"
        "},"
        "\"platformViews\":{"
        "\"previewDisabled\":{\"viewData\":{"
        "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":"
        "\"community\"}},"
        "\"message\":{\"body\":\"%s\"}"
        "}}"
        "}"
        "},"
        "\"createdDateTime\":\"%s\","
        "\"localNotificationId\":\"%s\""
        "}",
        escaped, escaped, created_at, notification_id);

    if(len < 0 || (size_t)len >= sizeof(payload)) {
        return -1;
    }

    {
        /*
         * Keep the exact payload on disk: the console's own log only reports
         * a length, and a malformed payload is registered without being
         * displayed (observed on hardware as an empty useCaseId).
         */
        FILE *dump = fopen("/data/psxterm/last-notification.json", "w");

        if(dump) {
            fwrite(payload, 1, (size_t)len, dump);
            fclose(dump);
        }
    }

    return sceNotificationSend(0xFE, 1, payload);
}

void
psx_platform_notify(const char *message)
{
    psx_notify_request_t req;

    if(!message || !*message) {
        return;
    }

    if(g_notify_available < 0) {
        g_notify_available =
            (sceNotificationSend != NULL ||
             sceKernelSendNotificationRequest != NULL)
                ? 1
                : 0;

        if(!g_notify_available) {
            PSX_LOGW("notifications unavailable on this system");
        }
    }

    if(!g_notify_available) {
        return;
    }

    if(notify_send_rich(message) == 0) {
        PSX_LOGI("notify: %s", message);
    }

    /*
     * Also post the legacy request.
     *
     * On the tested console the rich call succeeds - the shell logs Post7 and
     * registers the payload - but nothing is displayed, which points at a
     * firmware whose notification service does not render that template. The
     * legacy request is the one the console shows natively, so it is sent
     * unconditionally; where both work this costs one extra notification and
     * that is visible in the log.
     */
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", message);

    if(sceKernelSendNotificationRequest) {
        (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
        PSX_LOGI("notify (legacy): %s", message);
    }
}
