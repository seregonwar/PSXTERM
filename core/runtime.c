/*
 * PSXTerm runtime layout: every path an external CLI needs, defined once.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "psxterm/log.h"
#include "psxterm/platform.h"
#include "psxterm/runtime.h"
#include "psxterm/session.h"
#include "psxterm/util.h"

static char g_root[PSX_PATH_MAX];
static char g_base[PSX_PATH_MAX];
static char g_dirs[PSX_RUNTIME_DIR_COUNT][PSX_PATH_MAX];
static char g_ca_bundle[PSX_PATH_MAX];
static char g_manifest[PSX_PATH_MAX];
static bool g_initialized;

static const char *const g_dir_names[PSX_RUNTIME_DIR_COUNT] = {
    "bin",        "lib",  "etc",     "share", "tmp",
    "var",        "home", "cache",   "agents", "workspaces",
};

static void
runtime_init(void)
{
    const char *base;

    if(g_initialized) {
        return;
    }

    /*
     * The base can be pointed elsewhere for tests and custom deployments;
     * otherwise it is the platform's per-user home (on a console that is
     * /data/psxterm).
     */
    base = getenv("PSXTERM_RUNTIME_BASE");
    if(!base || !*base) {
        base = psx_platform_home_dir();
    }
    if(!base || !*base) {
        base = "/data/psxterm";
    }

    psx_path_join(g_root, sizeof(g_root), base, "runtime");
    snprintf(g_base, sizeof(g_base), "%s", base);

    for(size_t i = 0; i < PSX_RUNTIME_DIR_COUNT; i++) {
        const char *parent = (i == PSX_RUNTIME_DIR_AGENTS ||
                              i == PSX_RUNTIME_DIR_WORKSPACES)
                                 ? base
                                 : g_root;

        psx_path_join(g_dirs[i], sizeof(g_dirs[i]), parent, g_dir_names[i]);
    }

    psx_path_join(g_ca_bundle, sizeof(g_ca_bundle), g_root,
                  "etc/ca-bundle.crt");
    psx_path_join(g_manifest, sizeof(g_manifest), g_root, "runtime.json");

    g_initialized = true;
}

const char *
psx_runtime_root(void)
{
    runtime_init();

    return g_root;
}

const char *
psx_runtime_dir(psx_runtime_dir_t which)
{
    runtime_init();

    if(which < 0 || which >= PSX_RUNTIME_DIR_COUNT) {
        return g_root;
    }

    return g_dirs[which];
}

const char *
psx_runtime_ca_bundle(void)
{
    runtime_init();

    return g_ca_bundle;
}

const char *
psx_runtime_manifest_path(void)
{
    runtime_init();

    return g_manifest;
}

static bool
runtime_mkdir(const char *path)
{
    struct stat st;
    int saved_errno;

    if(mkdir(path, 0755) == 0) {
        return true;
    }
    if(errno == EEXIST) {
        if(stat(path, &st) == 0) {
            if(S_ISDIR(st.st_mode)) {
                return true;
            }
            errno = ENOTDIR;
        }
    }

    saved_errno = errno;
    PSX_LOGW("runtime: cannot create %s: %s", path, strerror(saved_errno));
    errno = saved_errno;
    return false;
}

bool
psx_runtime_prepare(void)
{
    runtime_init();

    /* The base first, then the root: mkdir does not create parents, and
     * everything else hangs off the root. */
    if(!runtime_mkdir(g_base)) {
        return false;
    }

    if(!runtime_mkdir(g_root)) {
        return false;
    }

    for(size_t i = 0; i < PSX_RUNTIME_DIR_COUNT; i++) {
        if(!runtime_mkdir(g_dirs[i])) {
            return false;
        }
    }

    /* The runtime home follows the XDG convention: agent state lives under
     * its .config and .local/share, cache under the runtime cache root. */
    {
        char nested[PSX_PATH_MAX];

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".config");
        if(!runtime_mkdir(nested)) {
            return false;
        }

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local");
        if(!runtime_mkdir(nested)) {
            return false;
        }

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local/share");
        if(!runtime_mkdir(nested)) {
            return false;
        }
    }

    PSX_NOTIFY("runtime ready at %s", g_root);

    return true;
}

size_t
psx_runtime_env_table(psx_runtime_env_t *out, size_t max)
{
    static char home_config[PSX_PATH_MAX];
    static char data_home[PSX_PATH_MAX];
    static char path_value[PSX_PATH_MAX];
    size_t written = 0;

    runtime_init();

    if(max == 0) {
        return 0;
    }

    psx_path_join(home_config, sizeof(home_config),
                  psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".config");
    psx_path_join(data_home, sizeof(data_home),
                  psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local/share");

    /*
     * PATH is a search list, not a path: the runtime bin comes first, then
     * the daemon's own bin directory. Joining them would have produced
     * ".../bin//data/psxterm/runtime/bin" (seen on hardware).
     */
    snprintf(path_value, sizeof(path_value), "%s:%s",
             psx_runtime_dir(PSX_RUNTIME_DIR_BIN), psx_platform_bin_dir());

#define ENTRY(key_, value_)                                                   \
    do {                                                                      \
        if(written < max) {                                                   \
            out[written].key = (key_);                                        \
            out[written].value = (value_);                                    \
            written++;                                                        \
        }                                                                     \
    } while(0)

    ENTRY("HOME", psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
    ENTRY("TMPDIR", psx_runtime_dir(PSX_RUNTIME_DIR_TMP));
    ENTRY("XDG_CONFIG_HOME", home_config);
    ENTRY("XDG_CACHE_HOME", psx_runtime_dir(PSX_RUNTIME_DIR_CACHE));
    ENTRY("XDG_DATA_HOME", data_home);
    ENTRY("PATH", path_value);
    ENTRY("SSL_CERT_FILE", g_ca_bundle);
    ENTRY("CURL_CA_BUNDLE", g_ca_bundle);
    ENTRY("TERM", "xterm-256color");

#undef ENTRY

    return written;
}

bool
psx_runtime_env_is_contract(const char *key)
{
    static const char *const keys[] = {
        "HOME",           "TMPDIR",         "XDG_CONFIG_HOME",
        "XDG_CACHE_HOME", "XDG_DATA_HOME",  "SSL_CERT_FILE",
        "CURL_CA_BUNDLE",
    };

    if(!key) {
        return false;
    }

    for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if(strcmp(key, keys[i]) == 0) {
            return true;
        }
    }

    return false;
}

size_t
psx_runtime_environment(char **out, size_t max)
{
    psx_runtime_env_t entries[16];
    size_t count;
    size_t written = 0;

    if(max == 0) {
        return 0;
    }
    if(!out) {
        errno = EINVAL;
        return 0;
    }

    count = psx_runtime_env_table(entries, sizeof(entries) / sizeof(entries[0]));
    while(written < count && written + 1 < max) {
        const char *key = entries[written].key;
        const char *value = entries[written].value;
        size_t len = strlen(key) + 1 + strlen(value) + 1;
        char *entry = malloc(len);

        if(!entry) {
            break;
        }
        snprintf(entry, len, "%s=%s", key, value);
        out[written++] = entry;
    }
    out[written] = NULL;

    return written;
}

static void
manifest_json_string(FILE *file, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    fputc('"', file);
    for(; *cursor; cursor++) {
        if(*cursor == '"' || *cursor == '\\') {
            fputc('\\', file);
            fputc(*cursor, file);
        } else if(*cursor < 0x20) {
            fprintf(file, "\\u%04x", (unsigned int)*cursor);
        } else {
            fputc(*cursor, file);
        }
    }
    fputc('"', file);
}

bool
psx_runtime_manifest_write(const char *extra_packages_json)
{
    FILE *file;
    int fd;
    int saved_errno = 0;
    bool initialize = !extra_packages_json || !*extra_packages_json;
    char temporary[PSX_PATH_MAX];
    const char *path = g_manifest;

    runtime_init();

    if(initialize) {
        fd = open(g_manifest, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(fd < 0 && errno == EEXIST) {
            struct stat st;

            if(stat(g_manifest, &st) == 0) {
                if(S_ISREG(st.st_mode)) {
                    return true;
                }
                errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
            }
        }
    } else {
        if(psx_path_join(temporary, sizeof(temporary), g_root,
                         ".runtime.json-XXXXXX") < 0) {
            return false;
        }
        path = temporary;
        fd = mkstemp(temporary);
    }
    if(fd < 0) {
        saved_errno = errno;
        PSX_LOGW("runtime: cannot write %s: %s", g_manifest,
                 strerror(saved_errno));
        errno = saved_errno;
        return false;
    }

    if(!(file = fdopen(fd, "w"))) {
        saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return false;
    }

    errno = 0;
    fprintf(file, "{\n");
    fprintf(file, "  \"version\": 1,\n");
    fprintf(file, "  \"platform\": ");
    manifest_json_string(file, psx_platform_id());
    fprintf(file, ",\n");
    fprintf(file, "  \"arch\": \"x86_64\",\n");
    fprintf(file, "  \"root\": ");
    manifest_json_string(file, g_root);
    fprintf(file, ",\n");
    fprintf(file, "  \"packages\": %s\n",
            initialize ? "{}" : extra_packages_json);
    fprintf(file, "}\n");

    if(ferror(file) || fflush(file) != 0 || fsync(fd) != 0) {
        saved_errno = errno ? errno : EIO;
    }
    if(fclose(file) != 0 && !saved_errno) {
        saved_errno = errno ? errno : EIO;
    }
    if(!saved_errno && !initialize && rename(temporary, g_manifest) != 0) {
        saved_errno = errno;
    }
    if(saved_errno) {
        unlink(path);
        PSX_LOGW("runtime: cannot write %s: %s", g_manifest,
                 strerror(saved_errno));
        errno = saved_errno;
        return false;
    }

    return true;
}
