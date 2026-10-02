#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "psxterm/diag.h"
#include "psxterm/instance.h"
#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/process.h"
#include "psxterm/session.h"
#include "psxterm/tty.h"
#include "psxterm/util.h"
#include "psxterm/version.h"

#define DIAG_DETAIL_SIZE PSX_DIAG_DETAIL_MAX
#define DIAG_CAPTURE_TTY 8192
#define DIAG_CAPTURE_STDERR 2048
#define DIAG_CLI_TIMEOUT_MS 5000
#define DIAG_CLI_SIGNAL_MS 250

/* ------------------------------------------------------------------ */
/* report bookkeeping                                                 */
/* ------------------------------------------------------------------ */

void
psx_diag_report_init(psx_diag_report_t *report)
{
    memset(report, 0, sizeof(*report));
}

static void
group_free(psx_diag_group_t *group)
{
    free(group->checks);
    memset(group, 0, sizeof(*group));
}

void
psx_diag_report_free(psx_diag_report_t *report)
{
    for(size_t i = 0; i < report->count; i++) {
        group_free(&report->groups[i]);
    }

    free(report->groups);
    memset(report, 0, sizeof(*report));
}

psx_diag_group_t *
psx_diag_group(psx_diag_report_t *report, const char *name)
{
    for(size_t i = 0; i < report->count; i++) {
        if(strcmp(report->groups[i].name, name) == 0) {
            return &report->groups[i];
        }
    }

    if(report->count == report->capacity) {
        size_t capacity = report->capacity ? report->capacity * 2 : 4;
        psx_diag_group_t *grown = realloc(report->groups,
                                          capacity * sizeof(*grown));

        if(!grown) {
            return NULL;
        }

        report->groups = grown;
        report->capacity = capacity;
    }

    {
        psx_diag_group_t *group = &report->groups[report->count++];

        memset(group, 0, sizeof(*group));
        snprintf(group->name, sizeof(group->name), "%s", name);

        return group;
    }
}

void
psx_diag_add(psx_diag_group_t *group, const char *name,
             psx_diag_status_t status, int error_code, const char *detail_fmt,
             ...)
{
    psx_diag_check_t *check;
    va_list ap;

    if(!group) {
        return;
    }

    if(group->count == group->capacity) {
        size_t capacity = group->capacity ? group->capacity * 2 : 8;
        psx_diag_check_t *grown = realloc(group->checks,
                                          capacity * sizeof(*grown));

        if(!grown) {
            return;
        }

        group->checks = grown;
        group->capacity = capacity;
    }

    check = &group->checks[group->count++];
    memset(check, 0, sizeof(*check));

    snprintf(check->name, sizeof(check->name), "%s", name);
    check->status = status;
    check->error_code = error_code;

    if(detail_fmt) {
        va_start(ap, detail_fmt);
        vsnprintf(check->detail, sizeof(check->detail), detail_fmt, ap);
        va_end(ap);
    }
}

const char *
psx_diag_status_name(psx_diag_status_t status)
{
    switch(status) {
    case PSX_DIAG_PASS: return "PASS";
    case PSX_DIAG_FAIL: return "FAIL";
    case PSX_DIAG_WARN: return "WARN";
    case PSX_DIAG_SKIP: return "SKIP";
    case PSX_DIAG_UNKNOWN: return "UNKNOWN";
    default: return "?";
    }
}

psx_diag_status_t
psx_diag_overall_status(const psx_diag_report_t *report)
{
    bool warn = false;

    for(size_t i = 0; i < report->count; i++) {
        const psx_diag_group_t *group = &report->groups[i];

        for(size_t j = 0; j < group->count; j++) {
            switch(group->checks[j].status) {
            case PSX_DIAG_FAIL:
                return PSX_DIAG_FAIL;
            case PSX_DIAG_WARN:
                warn = true;
                break;
            default:
                /* PASS, SKIP and UNKNOWN do not degrade the result: an
                 * undiscoverable value (for example firmware) is reported in
                 * the check itself, not as a failure of the system. */
                break;
            }
        }
    }

    return warn ? PSX_DIAG_WARN : PSX_DIAG_PASS;
}

const char *
psx_diag_overall_name(psx_diag_status_t overall)
{
    switch(overall) {
    case PSX_DIAG_PASS: return "READY";
    case PSX_DIAG_WARN: return "READY WITH WARNINGS";
    case PSX_DIAG_FAIL: return "NOT READY";
    default: return "UNKNOWN";
    }
}

int
psx_diag_exit_code(psx_diag_status_t overall)
{
    switch(overall) {
    case PSX_DIAG_FAIL: return PSX_DIAG_EXIT_FAILED;
    case PSX_DIAG_WARN: return PSX_DIAG_EXIT_WARNINGS;
    default: return PSX_DIAG_EXIT_READY;
    }
}

/* ------------------------------------------------------------------ */
/* rendering helpers                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *out;
    size_t cap;
    size_t len;
} diag_writer_t;

static void
writer_put(diag_writer_t *writer, const char *data, size_t len)
{
    if(writer->out && writer->len + 1 < writer->cap) {
        size_t room = writer->cap - writer->len - 1;
        size_t chunk = len < room ? len : room;

        memcpy(writer->out + writer->len, data, chunk);
    }

    writer->len += len;

    if(writer->out && writer->cap > 0) {
        size_t term = writer->len < writer->cap ? writer->len : writer->cap - 1;

        writer->out[term] = '\0';
    }
}

static void
writer_printf(diag_writer_t *writer, const char *fmt, ...)
{
    char buffer[512];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if(len < 0) {
        return;
    }

    if((size_t)len < sizeof(buffer)) {
        writer_put(writer, buffer, (size_t)len);
        return;
    }

    {
        char *big = malloc((size_t)len + 1);

        if(!big) {
            return;
        }

        va_start(ap, fmt);
        vsnprintf(big, (size_t)len + 1, fmt, ap);
        va_end(ap);

        writer_put(writer, big, (size_t)len);
        free(big);
    }
}

static const char *
firmware_or_unknown(void)
{
    const char *firmware = psx_platform_firmware();

    return firmware && *firmware ? firmware : "UNKNOWN";
}

size_t
psx_diag_format_human(const psx_diag_report_t *report, char *out, size_t out_cap)
{
    diag_writer_t writer = {.out = out, .cap = out_cap};
    psx_diag_status_t overall = psx_diag_overall_status(report);

    writer_printf(&writer, "PSXTerm diagnostics\n\n");
    writer_printf(&writer, "Platform:            %s\n", psx_platform_name());
    writer_printf(&writer, "PSXTerm version:     %s\n", PSXTERM_VERSION_STRING);
    writer_printf(&writer, "Protocol:            %s\n", PSXTERM_PROTOCOL_NAME);
    writer_printf(&writer, "Firmware:            %s\n", firmware_or_unknown());
    writer_printf(&writer, "Process backend:     %s\n",
                  psx_process_backend_name());
    writer_printf(&writer, "TTY backend:         %s\n\n",
                  psx_tty_backend_name(psx_tty_default_backend()));

    for(size_t i = 0; i < report->count; i++) {
        const psx_diag_group_t *group = &report->groups[i];

        writer_printf(&writer, "%s\n", group->name);

        for(size_t j = 0; j < group->count; j++) {
            const psx_diag_check_t *check = &group->checks[j];

            writer_printf(&writer, "  %-20s  %-7s", check->name,
                          psx_diag_status_name(check->status));

            if(check->detail[0]) {
                writer_printf(&writer, "  %s", check->detail);
            } else if(check->error_code) {
                writer_printf(&writer, "  errno %d", check->error_code);
            }
            writer_printf(&writer, "\n");
        }

        writer_printf(&writer, "\n");
    }

    writer_printf(&writer, "Result:\n  %s (%llu ms)\n",
                  psx_diag_overall_name(overall),
                  (unsigned long long)report->elapsed_ms);

    return writer.len;
}

static void
writer_json_string(diag_writer_t *writer, const char *value)
{
    writer_put(writer, "\"", 1);

    for(const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch(*p) {
        case '"': writer_put(writer, "\\\"", 2); break;
        case '\\': writer_put(writer, "\\\\", 2); break;
        case '\n': writer_put(writer, "\\n", 2); break;
        case '\r': writer_put(writer, "\\r", 2); break;
        case '\t': writer_put(writer, "\\t", 2); break;
        default:
            if(*p < 0x20) {
                writer_printf(writer, "\\u%04x", (unsigned)*p);
            } else {
                writer_put(writer, (const char *)p, 1);
            }
            break;
        }
    }

    writer_put(writer, "\"", 1);
}

size_t
psx_diag_format_json(const psx_diag_report_t *report, char *out, size_t out_cap)
{
    diag_writer_t writer = {.out = out, .cap = out_cap};
    psx_diag_status_t overall = psx_diag_overall_status(report);

    writer_printf(&writer, "{\n");
    writer_printf(&writer, "  \"platform\": ");
    writer_json_string(&writer, psx_platform_name());
    writer_printf(&writer, ",\n  \"version\": ");
    writer_json_string(&writer, PSXTERM_VERSION_STRING);
    writer_printf(&writer, ",\n  \"protocol\": ");
    writer_json_string(&writer, PSXTERM_PROTOCOL_NAME);
    writer_printf(&writer, ",\n  \"firmware\": ");
    writer_json_string(&writer, firmware_or_unknown());
    writer_printf(&writer, ",\n  \"process_backend\": ");
    writer_json_string(&writer, psx_process_backend_name());
    writer_printf(&writer, ",\n  \"tty_backend\": ");
    writer_json_string(&writer, psx_tty_backend_name(psx_tty_default_backend()));
    writer_printf(&writer, ",\n  \"result\": ");
    writer_json_string(&writer, psx_diag_overall_name(overall));
    writer_printf(&writer, ",\n  \"elapsed_ms\": %llu,\n",
                  (unsigned long long)report->elapsed_ms);
    writer_printf(&writer, "  \"groups\": [\n");

    for(size_t i = 0; i < report->count; i++) {
        const psx_diag_group_t *group = &report->groups[i];

        writer_printf(&writer, "    {\n      \"name\": ");
        writer_json_string(&writer, group->name);
        writer_printf(&writer, ",\n      \"checks\": [\n");

        for(size_t j = 0; j < group->count; j++) {
            const psx_diag_check_t *check = &group->checks[j];

            writer_printf(&writer, "        { \"name\": ");
            writer_json_string(&writer, check->name);
            writer_printf(&writer, ", \"status\": ");
            writer_json_string(&writer, psx_diag_status_name(check->status));
            writer_printf(&writer, ", \"error_code\": %d, \"detail\": ",
                          check->error_code);
            writer_json_string(&writer, check->detail);
            writer_printf(&writer, " }%s\n", j + 1 < group->count ? "," : "");
        }

        writer_printf(&writer, "      ]\n    }%s\n",
                      i + 1 < report->count ? "," : "");
    }

    writer_printf(&writer, "  ]\n}\n");

    return writer.len;
}

/* ------------------------------------------------------------------ */
/* platform / filesystem checks                                       */
/* ------------------------------------------------------------------ */

static void
check_platform(psx_diag_report_t *report)
{
    psx_diag_group_t *group = psx_diag_group(report, "Platform");
    bool backend = psx_process_backend_available();
    psx_tty_backend_t tty = psx_tty_default_backend();

    psx_diag_add(group, "identity", PSX_DIAG_PASS, 0, "%s (%s)",
                 psx_platform_name(), psx_platform_uname());
    psx_diag_add(group, "version", PSX_DIAG_PASS, 0, "%s, protocol %s",
                 PSXTERM_VERSION_STRING, PSXTERM_PROTOCOL_NAME);
    psx_diag_add(group, "firmware",
                 psx_platform_firmware() ? PSX_DIAG_PASS : PSX_DIAG_UNKNOWN, 0,
                 "%s", firmware_or_unknown());
    psx_diag_add(group, "process backend", backend ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 backend ? 0 : ENOSYS, "%s%s", psx_process_backend_name(),
                 backend ? "" : " (unavailable in this process)");
    psx_diag_add(group, "tty backend", PSX_DIAG_PASS, 0, "%s%s",
                 psx_tty_backend_name(tty),
                 tty == PSX_TTY_BACKEND_FREEBSD_PTY ? " (real pty)"
                                                    : " (fallback)");

    /*
     * Monotonic time drives every timeout in the daemon, so verify that the
     * clock actually advances instead of trusting clock_gettime.
     */
    {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
        uint64_t before = psx_now_ms();

        nanosleep(&pause, NULL);
        if(psx_now_ms() > before) {
            psx_diag_add(group, "clock", PSX_DIAG_PASS, 0,
                         "advances (%llu ms)", (unsigned long long)before);
        } else {
            psx_diag_add(group, "clock", PSX_DIAG_FAIL, 0,
                         "does not advance (timeouts would never fire)");
        }
    }

    /*
     * Instance replacement depends on reading our own payload name; a
     * system-like name disables it on purpose, so report what we see.
     */
    {
        char name[64];

        psx_instance_own_name(name, sizeof(name));

        if(name[0] && strncmp(name, "Sce", 3) != 0) {
            psx_diag_add(group, "process name", PSX_DIAG_PASS, 0, "%s", name);
        } else if(name[0]) {
            psx_diag_add(group, "process name", PSX_DIAG_WARN, 0,
                         "%s (system-like: replacement disabled)", name);
        } else {
            psx_diag_add(group, "process name", PSX_DIAG_UNKNOWN, 0,
                         "process enumeration unavailable");
        }
    }
}

static void
check_filesystem(psx_diag_report_t *report)
{
    psx_diag_group_t *group = psx_diag_group(report, "Filesystem");
    const char *home = psx_platform_home_dir();
    const char *bin = psx_platform_bin_dir();
    struct stat st;
    char path[PSX_PATH_MAX];
    char detail[DIAG_DETAIL_SIZE];
    uint32_t random = 0;
    int status = PSX_DIAG_FAIL;

    if(psx_platform_is_target()) {
        psx_diag_add(group, "/data",
                     access("/data", F_OK) == 0 ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                     errno, "%s", "/data");
        psx_diag_add(group, "/data writable",
                     access("/data", W_OK) == 0 ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                     errno, "%s", "/data");
    } else {
        psx_diag_add(group, "/data", PSX_DIAG_SKIP, 0,
                     "not applicable on the host");
    }

    if(!home || !*home) {
        psx_diag_add(group, "home directory", PSX_DIAG_FAIL, ENOENT,
                     "no home directory configured");
    } else if(stat(home, &st) != 0 || !S_ISDIR(st.st_mode)) {
        psx_diag_add(group, "home directory", PSX_DIAG_FAIL, errno, "%s", home);
    } else {
        psx_diag_add(group, "home directory", PSX_DIAG_PASS, 0, "%s", home);
    }

    if(home && access(home, W_OK) == 0) {
        psx_diag_add(group, "home writable", PSX_DIAG_PASS, 0, "%s", home);
    } else {
        psx_diag_add(group, "home writable", PSX_DIAG_FAIL, errno, "%s",
                     home ? home : "(unset)");
    }

    if(!psx_platform_is_target()) {
        psx_diag_add(group, "bin directory", PSX_DIAG_SKIP, 0,
                     "not applicable on the host");
    } else if(bin && stat(bin, &st) == 0 && S_ISDIR(st.st_mode)) {
        psx_diag_add(group, "bin directory", PSX_DIAG_PASS, 0, "%s", bin);
    } else {
        psx_diag_add(group, "bin directory", PSX_DIAG_WARN, errno,
                     "%s does not exist; external tools must be installed "
                     "there",
                     bin ? bin : "(unset)");
    }

    /* Unique temporary file; never touches existing user files. */
    if(!home || !*home) {
        psx_diag_add(group, "temp file roundtrip", PSX_DIAG_SKIP, 0,
                     "no home directory");
        return;
    }

    if(psx_platform_random_bytes(&random, sizeof(random)) < 0) {
        random = (uint32_t)psx_now_ms();
    }

    snprintf(detail, sizeof(detail), ".psxterm-diag-%d-%08x", (int)getpid(),
             (unsigned)random);

    if(psx_path_join(path, sizeof(path), home, detail) < 0) {
        psx_diag_add(group, "temp file roundtrip", PSX_DIAG_FAIL, ENAMETOOLONG,
                     "%s", detail);
        return;
    }

    {
        const char payload[] = "psxterm-diagnostic\n";
        char readback[sizeof(payload)];
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        ssize_t written = -1;
        ssize_t got = -1;

        if(fd < 0) {
            psx_diag_add(group, "temp file roundtrip", PSX_DIAG_FAIL, errno,
                         "create %s failed", detail);
            return;
        }

        written = write(fd, payload, sizeof(payload));
        close(fd);

        if(written != (ssize_t)sizeof(payload)) {
            psx_diag_add(group, "temp file roundtrip", PSX_DIAG_FAIL, errno,
                         "write %s failed", detail);
            unlink(path);
            return;
        }

        if((fd = open(path, O_RDONLY)) >= 0) {
            got = read(fd, readback, sizeof(readback));
            close(fd);
        }

        if(got == (ssize_t)sizeof(payload) &&
           memcmp(readback, payload, sizeof(payload)) == 0) {
            status = PSX_DIAG_PASS;
        }

        if(unlink(path) != 0 && status == PSX_DIAG_PASS) {
            psx_diag_add(group, "temp file roundtrip", PSX_DIAG_WARN, errno,
                         "read/write OK but unlink %s failed", detail);
            return;
        }

        psx_diag_add(group, "temp file roundtrip", status,
                     status == PSX_DIAG_PASS ? 0 : errno,
                     status == PSX_DIAG_PASS ? "%s" : "roundtrip failed for %s",
                     detail);
    }
}

/* ------------------------------------------------------------------ */
/* tty checks (reuse the runtime probe, do not duplicate it)          */
/* ------------------------------------------------------------------ */

static void
check_tty(psx_diag_report_t *report)
{
    psx_diag_group_t *group = psx_diag_group(report, "TTY");
    psx_tty_probe_result_t probe;

    psx_tty_probe(&probe);

    psx_diag_add(group, "/dev/ptmx", probe.ptmx_open ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.ptmx_open ? 0 : errno, "%s",
                 probe.ptmx_open ? "/dev/ptmx open" : probe.detail);
    psx_diag_add(group, "TIOCGPTN", probe.tiocgptn ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.tiocgptn ? 0 : errno, "%s",
                 probe.tiocgptn ? "pts number reported" : probe.detail);
    psx_diag_add(group, "PTY slave", probe.slave_open ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.slave_open ? 0 : errno, "%s",
                 probe.slave_open ? "/dev/pts/<n> open" : probe.detail);
    psx_diag_add(group, "termios", probe.termios ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.termios ? 0 : errno, "%s",
                 probe.termios ? "tcgetattr/tcsetattr" : probe.detail);
    psx_diag_add(group, "TIOCSWINSZ", probe.winsize ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.winsize ? 0 : errno, "%s",
                 probe.winsize ? "window size read/write" : probe.detail);
    psx_diag_add(group, "isatty", probe.isatty_true ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.isatty_true ? 0 : errno, "%s",
                 probe.isatty_true ? "slave reported as a tty" : probe.detail);
    psx_diag_add(group, "I/O roundtrip",
                 probe.io_roundtrip ? PSX_DIAG_PASS : PSX_DIAG_FAIL,
                 probe.io_roundtrip ? 0 : errno, "%s",
                 probe.io_roundtrip ? "master -> slave byte roundtrip"
                                    : probe.detail);
    psx_diag_add(group, "selected backend", PSX_DIAG_PASS, 0, "%s",
                 psx_tty_backend_name(psx_tty_default_backend()));
}

/* ------------------------------------------------------------------ */
/* session / socket checks                                            */
/* ------------------------------------------------------------------ */

static void
format_endpoint(const struct sockaddr *addr, socklen_t len, char *out,
                size_t out_cap)
{
    char host[64] = "?";
    unsigned port = 0;

    if(addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;

        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if(addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;

        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    }

    snprintf(out, out_cap, "%s:%u", host, port);
    (void)len;
}

static void
format_capabilities(uint32_t caps, char *out, size_t out_cap)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {
        {PTTY_CAP_REAL_PTY, "real-pty"},
        {PTTY_CAP_PIPE_TTY, "pipe-tty"},
        {PTTY_CAP_EXEC, "exec"},
        {PTTY_CAP_FILE_TRANSFER, "file-transfer"},
        {PTTY_CAP_SESSION_RESUME, "session-resume"},
        {PTTY_CAP_JOB_CONTROL, "job-control"},
        {PTTY_CAP_AUTH_CHALLENGE, "auth-challenge"},
        {PTTY_CAP_COMPRESSION, "compression"},
        {PTTY_CAP_JSON_DIAGNOSTICS, "json-diagnostics"},
    };
    size_t used = 0;

    out[0] = '\0';

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if(!(caps & names[i].bit)) {
            continue;
        }

        snprintf(out + used, out_cap - used, "%s%s", used ? " " : "",
                 names[i].name);
        used = strlen(out);
    }

    if(!used) {
        snprintf(out, out_cap, "none");
    }
}

static void
check_session(psx_diag_report_t *report, const psx_session_t *session)
{
    psx_diag_group_t *group = psx_diag_group(report, "Session");

    if(!session) {
        psx_diag_add(group, "client session", PSX_DIAG_SKIP, 0,
                     "local mode: no client connected");
        return;
    }

    {
        struct sockaddr_storage local;
        struct sockaddr_storage peer;
        socklen_t local_len = sizeof(local);
        socklen_t peer_len = sizeof(peer);
        char local_text[96] = "?";
        char peer_text[96] = "?";

        if(getsockname(session->sock_fd, (struct sockaddr *)&local,
                       &local_len) == 0 &&
           getpeername(session->sock_fd, (struct sockaddr *)&peer,
                       &peer_len) == 0) {
            format_endpoint((struct sockaddr *)&local, local_len, local_text,
                            sizeof(local_text));
            format_endpoint((struct sockaddr *)&peer, peer_len, peer_text,
                            sizeof(peer_text));
            psx_diag_add(group, "socket", PSX_DIAG_PASS, 0, "local %s <- %s",
                         local_text, peer_text);
        } else {
            psx_diag_add(group, "socket", PSX_DIAG_FAIL, errno,
                         "getsockname/getpeername failed");
        }
    }

    {
        int nodelay = 0;
        socklen_t len = sizeof(nodelay);

        if(getsockopt(session->sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                      &len) == 0) {
            psx_diag_add(group, "TCP_NODELAY",
                         nodelay ? PSX_DIAG_PASS : PSX_DIAG_WARN, 0,
                         nodelay ? "enabled" : "disabled");
        } else {
            psx_diag_add(group, "TCP_NODELAY", PSX_DIAG_WARN, errno,
                         "getsockopt failed");
        }
    }

    psx_diag_add(group, "session", PSX_DIAG_PASS, 0,
                 "id %u, client \"%s\", state %s", session->id,
                 session->client_name[0] ? session->client_name : "-",
                 psx_session_state_name(session->state));

    psx_diag_add(group, "frames",
                 session->protocol_errors ? PSX_DIAG_WARN : PSX_DIAG_PASS,
                 session->protocol_errors ? EPROTO : 0,
                 "in=%llu out=%llu protocol_errors=%u",
                 (unsigned long long)session->frames_in,
                 (unsigned long long)session->frames_out,
                 session->protocol_errors);

    {
        char caps[128];

        format_capabilities(session->capabilities, caps, sizeof(caps));
        psx_diag_add(group, "capabilities", PSX_DIAG_PASS, 0, "0x%08x: %s",
                     session->capabilities, caps);
    }

    psx_diag_add(group, "backpressure", PSX_DIAG_PASS, 0,
                 "input_events=%llu discarded_bytes=%llu",
                 (unsigned long long)session->input_backpressure_events,
                 (unsigned long long)session->input_discarded_bytes);

    psx_diag_add(group, "foreground process", PSX_DIAG_PASS, 0, "%s",
                 session->proc.running ? "running" : "none");
    if(session->proc.running) {
        psx_diag_add(group, "foreground pid", PSX_DIAG_PASS, 0, "%d",
                     (int)session->proc.pid);
    }
}

/* ------------------------------------------------------------------ */
/* process execution checks                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char tty_out[DIAG_CAPTURE_TTY];
    size_t tty_len;
    size_t tty_total;
    char stderr_out[DIAG_CAPTURE_STDERR];
    size_t stderr_len;
    size_t stderr_total;
    int exit_status;
    bool exited;
    pid_t pid;
} diag_capture_t;

typedef struct {
    const char *path;
    char *const *argv;
    const char *stdin_line;
    bool separate_stderr;
    uint16_t rows;
    uint16_t cols;
    int timeout_ms;
    int signal_after_ms;
    int signal_number;
} diag_cli_opts_t;

static void
capture_append(char *buffer, size_t cap, size_t *len, const char *data,
               size_t n)
{
    if(n > cap - *len) {
        n = cap - *len;
    }

    memcpy(buffer + *len, data, n);
    *len += n;
    buffer[*len] = '\0';
}

static void
capture_drain(int fd, char *buffer, size_t cap, size_t *len, size_t *total)
{
    for(;;) {
        char chunk[1024];
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            /* EAGAIN: nothing left; EIO: PTY master with no slave left. */
            return;
        }
        if(n == 0) {
            return;
        }

        *total += (size_t)n;
        capture_append(buffer, cap, len, chunk, (size_t)n);
    }
}

static int
write_fd_all(int fd, const char *data, size_t len)
{
    size_t offset = 0;
    int attempts = 0;

    while(offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);

        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if((errno == EAGAIN || errno == EWOULDBLOCK) && ++attempts < 50) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};

                poll(&pfd, 1, 20);
                continue;
            }
            return -1;
        }

        offset += (size_t)n;
    }

    return 0;
}

/*
 * Runs cli_test through the real process backend on a dedicated tty and
 * captures what the process produced.
 */
static int
diag_cli_run(const diag_cli_opts_t *opts, diag_capture_t *capture,
             psx_spawn_failure_t *failure, char *error, size_t error_cap)
{
    static const char *const env_fixed[] = {"PSXTERM_DOCTOR=1",
                                            "TERM=xterm-256color", NULL};
    char path_env[PSX_PATH_MAX];
    char *envp[4];
    psx_tty_t tty;
    psx_spawn_options_t spawn;
    psx_spawn_failure_t local_failure;
    int stderr_pipe[2] = {-1, -1};
    uint64_t start;
    bool signalled = false;
    int rc = -1;

    memset(capture, 0, sizeof(*capture));
    memset(&local_failure, 0, sizeof(local_failure));
    snprintf(path_env, sizeof(path_env), "PATH=%s", psx_platform_default_path());

    envp[0] = (char *)env_fixed[0];
    envp[1] = (char *)env_fixed[1];
    envp[2] = path_env;
    envp[3] = NULL;

    psx_tty_init(&tty);

    if(psx_tty_open(&tty, psx_tty_default_backend(), opts->rows, opts->cols) < 0) {
        snprintf(error, error_cap, "tty open failed: %s", strerror(errno));
        return -1;
    }

    if(opts->separate_stderr) {
        if(pipe(stderr_pipe) < 0) {
            snprintf(error, error_cap, "pipe failed: %s", strerror(errno));
            psx_tty_close(&tty);
            return -1;
        }

        psx_set_cloexec(stderr_pipe[0], true);
        psx_set_cloexec(stderr_pipe[1], true);
        psx_set_nonblocking(stderr_pipe[0], true);
    }

    memset(&spawn, 0, sizeof(spawn));
    spawn.path = opts->path;
    spawn.argv = opts->argv;
    spawn.envp = envp;
    spawn.stdin_fd = tty.slave_fd;
    spawn.stdout_fd = tty.slave_fd;
    spawn.stderr_fd = opts->separate_stderr ? stderr_pipe[1] : tty.slave_fd;

    capture->pid = psx_spawn_ex(&spawn, failure ? failure : &local_failure);

    if(capture->pid < 0) {
        snprintf(error, error_cap, "%s", strerror(errno));
        goto out;
    }

    if(opts->separate_stderr) {
        close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
    }

    if(opts->stdin_line) {
        char line[256];

        snprintf(line, sizeof(line), "%s\n", opts->stdin_line);
        if(write_fd_all(tty.master_fd, line, strlen(line)) < 0) {
            snprintf(error, error_cap, "stdin write failed: %s",
                     strerror(errno));
            psx_process_kill(capture->pid, SIGKILL);
            goto reap;
        }

        /*
         * Deliver the end of input, exactly as a client does when its own
         * stdin closes: without it a payload parked in a read never finishes
         * and the run looks silent. Checks that mean to keep the process
         * running (the signal check) deliberately do not send stdin.
         */
        if(psx_tty_send_eof(&tty) < 0) {
            PSX_LOGD("diagnostics: cannot signal end of input (%s)",
                     strerror(errno));
        }
    }

    start = psx_now_ms();

    for(;;) {
        struct pollfd pfds[2];
        nfds_t count = 0;

        pfds[count].fd = tty.master_fd;
        pfds[count].events = POLLIN;
        count++;

        if(stderr_pipe[0] >= 0) {
            pfds[count].fd = stderr_pipe[0];
            pfds[count].events = POLLIN;
            count++;
        }

        poll(pfds, count, 50);

        if(pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            capture_drain(tty.master_fd, capture->tty_out,
                          sizeof(capture->tty_out), &capture->tty_len,
                          &capture->tty_total);
        }
        if(count > 1 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            capture_drain(stderr_pipe[0], capture->stderr_out,
                          sizeof(capture->stderr_out), &capture->stderr_len,
                          &capture->stderr_total);
        }

        {
            int wait_rc = psx_process_wait(capture->pid, &capture->exit_status,
                                           0);

            if(wait_rc == 0) {
                capture->exited = true;
                rc = 0;
                break;
            }
            if(wait_rc < 0) {
                snprintf(error, error_cap, "wait failed: %s", strerror(errno));
                break;
            }
        }

        if(opts->signal_after_ms > 0 && !signalled &&
           psx_now_ms() - start >= (uint64_t)opts->signal_after_ms) {
            psx_process_kill(capture->pid, opts->signal_number);
            signalled = true;
        }

        if(psx_now_ms() - start > (uint64_t)opts->timeout_ms) {
            snprintf(error, error_cap, "process did not exit within %d ms",
                     opts->timeout_ms);
            psx_process_kill(capture->pid, SIGKILL);
            break;
        }
    }

    /* Give the last output a chance to arrive. */
    capture_drain(tty.master_fd, capture->tty_out, sizeof(capture->tty_out),
                  &capture->tty_len, &capture->tty_total);
    if(stderr_pipe[0] >= 0) {
        capture_drain(stderr_pipe[0], capture->stderr_out,
                      sizeof(capture->stderr_out), &capture->stderr_len,
                      &capture->stderr_total);
    }

reap:
    if(!capture->exited) {
        psx_process_wait(capture->pid, &capture->exit_status, 500);
    }

out:
    if(stderr_pipe[0] >= 0) {
        close(stderr_pipe[0]);
    }
    if(stderr_pipe[1] >= 0) {
        close(stderr_pipe[1]);
    }
    psx_tty_close(&tty);

    return rc;
}

static bool
diag_locate_cli_test(char *out, size_t out_cap, char *searched, size_t searched_cap)
{
    char candidate[PSX_PATH_MAX];
    char exe[PSX_PATH_MAX];
    const char *bin = psx_platform_bin_dir();
    ssize_t n;
    char *slash;

    searched[0] = '\0';

    if(bin && *bin) {
        snprintf(searched + strlen(searched), searched_cap - strlen(searched),
                 "%s/cli_test.elf, %s/cli_test, ", bin, bin);

        if(psx_path_join(candidate, sizeof(candidate), bin, "cli_test.elf") == 0 &&
           access(candidate, F_OK) == 0) {
            snprintf(out, out_cap, "%s", candidate);
            return true;
        }
        if(psx_path_join(candidate, sizeof(candidate), bin, "cli_test") == 0 &&
           access(candidate, F_OK) == 0) {
            snprintf(out, out_cap, "%s", candidate);
            return true;
        }
    }

    /* Next to the running executable: /proc/curproc/file on FreeBSD
     * (PS4/PS5), /proc/self/exe on Linux. */
    n = readlink("/proc/curproc/file", exe, sizeof(exe) - 1);
    if(n < 0) {
        n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    }

    if(n > 0) {
        exe[n] = '\0';
        if((slash = strrchr(exe, '/'))) {
            *slash = '\0';
            snprintf(searched + strlen(searched), searched_cap - strlen(searched),
                     "%s/cli_test, %s/cli_test.elf, ", exe, exe);

            if(psx_path_join(candidate, sizeof(candidate), exe, "cli_test") == 0 &&
               access(candidate, F_OK) == 0) {
                snprintf(out, out_cap, "%s", candidate);
                return true;
            }
            if(psx_path_join(candidate, sizeof(candidate), exe, "cli_test.elf") == 0 &&
               access(candidate, F_OK) == 0) {
                snprintf(out, out_cap, "%s", candidate);
                return true;
            }
        }
    }

    snprintf(searched + strlen(searched), searched_cap - strlen(searched),
             "./cli_test, ./cli_test.elf");

    if(access("cli_test", F_OK) == 0) {
        snprintf(out, out_cap, "%s", "cli_test");
        return true;
    }
    if(access("cli_test.elf", F_OK) == 0) {
        snprintf(out, out_cap, "%s", "cli_test.elf");
        return true;
    }

    return false;
}

static void
add_marker_check(psx_diag_group_t *group, const char *name, const char *haystack,
                 const char *marker)
{
    if(strstr(haystack, marker)) {
        psx_diag_add(group, name, PSX_DIAG_PASS, 0, "observed \"%s\"", marker);
    } else {
        psx_diag_add(group, name, PSX_DIAG_FAIL, 0, "missing \"%s\"", marker);
    }
}

static void
skip_process_checks(psx_diag_group_t *group, const char *reason,
                    bool include_spawn)
{
    static const char *const names[] = {
        "spawn",    "argv",   "argv[2] quoting", "environment", "stdin",
        "stdout",   "stderr", "isatty",           "resize",      "exit status",
        "stdio separation",   "SIGINT",
    };

    for(size_t i = include_spawn ? 0 : 1;
        i < sizeof(names) / sizeof(names[0]); i++) {
        psx_diag_add(group, names[i], PSX_DIAG_SKIP, 0, "%s", reason);
    }
}

static void
check_process(psx_diag_report_t *report)
{
    psx_diag_group_t *group = psx_diag_group(report, "Process execution");
    char cli_test[PSX_PATH_MAX];
    char searched[DIAG_DETAIL_SIZE];
    char detail[256];
    diag_cli_opts_t opts;
    diag_capture_t capture;
    psx_spawn_failure_t failure;
    char **argv;

    /*
     * The argument vector lives on the heap, not on this frame: the process
     * created for the payload only sees heap and read-only data reliably, so
     * stack strings and a stack array reach it as garbage (hardware-verified:
     * the payload reported argc=16 with unreadable arguments).
     */
    argv = calloc(4, sizeof(*argv));
    if(!argv) {
        skip_process_checks(group, "out of memory", true);
        return;
    }
    argv[0] = strdup("cli_test");
    argv[1] = strdup("doctor");
    argv[2] = strdup("beta gamma");
    argv[3] = NULL;

    if(!argv[0] || !argv[1] || !argv[2]) {
        free(argv[0]);
        free(argv[1]);
        free(argv[2]);
        free(argv);
        skip_process_checks(group, "out of memory", true);
        return;
    }

    if(!diag_locate_cli_test(cli_test, sizeof(cli_test), searched,
                             sizeof(searched))) {
        psx_diag_add(group, "cli_test located", PSX_DIAG_FAIL, ENOENT,
                     "not found; searched %s", searched);
        skip_process_checks(group, "cli_test is not available", true);
        return;
    }

    psx_diag_add(group, "cli_test located", PSX_DIAG_PASS, 0, "%s", cli_test);

    memset(&opts, 0, sizeof(opts));
    memset(&failure, 0, sizeof(failure));

    opts.path = cli_test;
    opts.argv = (char *const *)argv;
    opts.stdin_line = "doctor-stdin";
    opts.separate_stderr = false;
    opts.rows = 31;
    opts.cols = 101;
    opts.timeout_ms = DIAG_CLI_TIMEOUT_MS;

    if(diag_cli_run(&opts, &capture, &failure, detail, sizeof(detail)) < 0) {
        psx_diag_add(group, "spawn", PSX_DIAG_FAIL, failure.error_code,
                     "stage %s: %s", psx_spawn_stage_name(failure.stage),
                     failure.detail[0] ? failure.detail : detail);
        skip_process_checks(group, "spawn failed", false);
        return;
    }

    psx_diag_add(group, "spawn", PSX_DIAG_PASS, 0, "pid %d, stage %s",
                 (int)capture.pid, psx_spawn_stage_name(failure.stage));

    /* Byte counts separate "the capture is empty" from "the process never
     * wrote", which the marker checks alone cannot tell apart. */
    psx_diag_add(group, "captured output", PSX_DIAG_PASS, 0,
                 "tty=%zu bytes, stderr=%zu bytes",
                 (size_t)capture.tty_total, (size_t)capture.stderr_total);

    /* An escaped sample of what actually arrived: when a marker check fails
     * this says whether the payload wrote something else or nothing usable. */
    {
        char preview[160];
        size_t n = 0;

        for(size_t i = 0; i < capture.tty_len && n + 5 < sizeof(preview); i++) {
            unsigned char c = (unsigned char)capture.tty_out[i];

            if(c == '\n') {
                preview[n++] = '\\';
                preview[n++] = 'n';
            } else if(c == '\r') {
                preview[n++] = '\\';
                preview[n++] = 'r';
            } else if(c < 32 || c > 126) {
                preview[n++] = '\\';
                preview[n++] = 'x';
                preview[n++] = (char)('0' + ((c >> 4) & 0xf));
                preview[n++] = (char)('0' + (c & 0xf));
            } else {
                preview[n++] = (char)c;
            }
        }
        preview[n] = '\0';

        psx_diag_add(group, "capture sample", PSX_DIAG_PASS, 0, "%s", preview);
    }

    add_marker_check(group, "argv", capture.tty_out, "argv[1]=doctor");
    if(strstr(capture.tty_out, "argv[2]=beta gamma")) {
        psx_diag_add(group, "argv[2] quoting", PSX_DIAG_PASS, 0, "%s",
                     "quoted argument preserved");
    } else {
        psx_diag_add(group, "argv[2] quoting", PSX_DIAG_FAIL, 0, "%s",
                     "quoted argument lost");
    }

    add_marker_check(group, "environment", capture.tty_out,
                     "env PSXTERM_DOCTOR=1");
    add_marker_check(group, "stdin", capture.tty_out, "read=doctor-stdin");
    add_marker_check(group, "stdout", capture.tty_out,
                     "cli_test: exiting with 7");
    add_marker_check(group, "stderr", capture.tty_out,
                     "cli_test: stderr works");

    if(strstr(capture.tty_out, "isatty: stdin=1 stdout=1 stderr=1")) {
        psx_diag_add(group, "isatty", PSX_DIAG_PASS, 0, "%s",
                     "stdin/stdout/stderr are a terminal");
    } else if(strstr(capture.tty_out, "isatty: stdin=0 stdout=0 stderr=0")) {
        psx_diag_add(group, "isatty", PSX_DIAG_WARN, 0,
                     "no terminal (PipeTTY fallback)");
    } else {
        psx_diag_add(group, "isatty", PSX_DIAG_FAIL, 0, "%s",
                     "isatty output not observed");
    }

    if(strstr(capture.tty_out, "winsize: rows=31 cols=101")) {
        psx_diag_add(group, "resize", PSX_DIAG_PASS, 0, "%s",
                     "31x101 applied to the process");
    } else {
        psx_diag_add(group, "resize", PSX_DIAG_FAIL, 0, "%s",
                     "31x101 was not reported by the process");
    }

    if(capture.exited && WIFEXITED(capture.exit_status) &&
       WEXITSTATUS(capture.exit_status) == 7) {
        psx_diag_add(group, "exit status", PSX_DIAG_PASS, 0, "%s", "7");
    } else if(capture.exited && WIFEXITED(capture.exit_status) &&
              WEXITSTATUS(capture.exit_status) == 127) {
        psx_diag_add(group, "exit status", PSX_DIAG_FAIL, 0,
                     "127 (exec failed inside the spawn stage)");
    } else if(capture.exited && WIFSIGNALED(capture.exit_status)) {
        psx_diag_add(group, "exit status", PSX_DIAG_FAIL, 0, "signal %d",
                     WTERMSIG(capture.exit_status));
    } else {
        psx_diag_add(group, "exit status", PSX_DIAG_FAIL, 0, "%s",
                     "process did not exit normally");
    }

    /* Second run: prove stdout and stderr are separate descriptors. */
    memset(&opts, 0, sizeof(opts));
    memset(&failure, 0, sizeof(failure));

    opts.path = cli_test;
    opts.argv = argv;
    opts.stdin_line = "separation";
    opts.separate_stderr = true;
    opts.rows = 24;
    opts.cols = 80;
    opts.timeout_ms = DIAG_CLI_TIMEOUT_MS;

    if(diag_cli_run(&opts, &capture, &failure, detail, sizeof(detail)) < 0) {
        psx_diag_add(group, "stdio separation", PSX_DIAG_FAIL,
                     failure.error_code, "stage %s: %s",
                     psx_spawn_stage_name(failure.stage), detail);
    } else if(strstr(capture.stderr_out, "cli_test: stderr works") &&
              !strstr(capture.tty_out, "cli_test: stderr works") &&
              strstr(capture.tty_out, "cli_test: exiting with 7") &&
              !strstr(capture.stderr_out, "cli_test: exiting with 7")) {
        psx_diag_add(group, "stdio separation", PSX_DIAG_PASS, 0,
                     "stderr arrived on its own descriptor (%u bytes)",
                     (unsigned)capture.stderr_total);
    } else {
        psx_diag_add(group, "stdio separation", PSX_DIAG_FAIL, 0,
                     "stderr/stdout could not be separated");
    }

    /* Third run: signal delivery. */
    memset(&opts, 0, sizeof(opts));
    memset(&failure, 0, sizeof(failure));

    opts.path = cli_test;
    opts.argv = argv;
    opts.stdin_line = NULL;
    opts.separate_stderr = false;
    opts.rows = 24;
    opts.cols = 80;
    opts.timeout_ms = DIAG_CLI_TIMEOUT_MS;
    opts.signal_after_ms = DIAG_CLI_SIGNAL_MS;
    opts.signal_number = SIGINT;

    if(diag_cli_run(&opts, &capture, &failure, detail, sizeof(detail)) < 0) {
        psx_diag_add(group, "SIGINT", PSX_DIAG_FAIL, failure.error_code,
                     "stage %s: %s", psx_spawn_stage_name(failure.stage),
                     detail);
    } else if(capture.exited && WIFSIGNALED(capture.exit_status) &&
              WTERMSIG(capture.exit_status) == SIGINT) {
        psx_diag_add(group, "SIGINT", PSX_DIAG_PASS, 0, "%s",
                     "process terminated by SIGINT");
    } else if(capture.exited && WIFEXITED(capture.exit_status) &&
              WEXITSTATUS(capture.exit_status) == 130) {
        psx_diag_add(group, "SIGINT", PSX_DIAG_PASS, 0, "%s", "exit status 130");
    } else {
        psx_diag_add(group, "SIGINT", PSX_DIAG_FAIL, 0, "%s",
                     "SIGINT did not terminate the process");
    }
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

void
psx_diag_run(psx_diag_report_t *report, const psx_session_t *session)
{
    uint64_t start = psx_now_ms();

    PSX_LOGI("diagnostics: starting (session %s)",
             session ? "attached" : "local");

    check_platform(report);
    check_filesystem(report);
    check_tty(report);
    check_session(report, session);
    check_process(report);

    report->elapsed_ms = psx_now_ms() - start;

    PSX_LOGI("diagnostics: finished in %llu ms, result %s",
             (unsigned long long)report->elapsed_ms,
             psx_diag_overall_name(psx_diag_overall_status(report)));
}
