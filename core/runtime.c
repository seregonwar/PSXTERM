/*
 * PSXTerm runtime layout: every path an external CLI needs, defined once.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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

bool
psx_runtime_prepare(void)
{
    runtime_init();

    /* The base first, then the root: mkdir does not create parents, and
     * everything else hangs off the root. */
    if(mkdir(g_base, 0755) != 0 && errno != EEXIST) {
        PSX_LOGW("runtime: cannot create %s: %s", g_base, strerror(errno));
        return false;
    }

    if(mkdir(g_root, 0755) != 0 && errno != EEXIST) {
        PSX_LOGW("runtime: cannot create %s: %s", g_root, strerror(errno));
        return false;
    }

    for(size_t i = 0; i < PSX_RUNTIME_DIR_COUNT; i++) {
        if(mkdir(g_dirs[i], 0755) != 0 && errno != EEXIST) {
            PSX_LOGW("runtime: cannot create %s: %s", g_dirs[i],
                     strerror(errno));
            return false;
        }
    }

    /* The runtime home follows the XDG convention: agent state lives under
     * its .config and .local/share, cache under the runtime cache root. */
    {
        char nested[PSX_PATH_MAX];

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".config");
        if(mkdir(nested, 0755) != 0 && errno != EEXIST) {
            PSX_LOGW("runtime: cannot create %s: %s", nested, strerror(errno));
            return false;
        }

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local");
        if(mkdir(nested, 0755) != 0 && errno != EEXIST) {
            PSX_LOGW("runtime: cannot create %s: %s", nested, strerror(errno));
            return false;
        }

        psx_path_join(nested, sizeof(nested),
                      psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local/share");
        if(mkdir(nested, 0755) != 0 && errno != EEXIST) {
            PSX_LOGW("runtime: cannot create %s: %s", nested, strerror(errno));
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
    psx_path_join(path_value, sizeof(path_value), psx_platform_bin_dir(),
                  psx_runtime_dir(PSX_RUNTIME_DIR_BIN));

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

static void
env_add(char **out, size_t max, size_t *written, const char *key,
        const char *value)
{
    size_t len;
    char *entry;

    if(*written >= max) {
        return;
    }

    len = strlen(key) + 1 + strlen(value) + 1;
    if(!(entry = malloc(len))) {
        return;
    }

    snprintf(entry, len, "%s=%s", key, value);
    out[*written] = entry;
    (*written)++;
}

size_t
psx_runtime_environment(char **out, size_t max)
{
    size_t written = 0;
    char path[PSX_PATH_MAX];

    runtime_init();

    if(max == 0) {
        return 0;
    }

    env_add(out, max, &written, "HOME",
            psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
    env_add(out, max, &written, "TMPDIR",
            psx_runtime_dir(PSX_RUNTIME_DIR_TMP));

    psx_path_join(path, sizeof(path),
                  psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".config");
    env_add(out, max, &written, "XDG_CONFIG_HOME", path);

    env_add(out, max, &written, "XDG_CACHE_HOME",
            psx_runtime_dir(PSX_RUNTIME_DIR_CACHE));

    psx_path_join(path, sizeof(path),
                  psx_runtime_dir(PSX_RUNTIME_DIR_HOME), ".local/share");
    env_add(out, max, &written, "XDG_DATA_HOME", path);

    psx_path_join(path, sizeof(path), psx_platform_bin_dir(),
                  psx_runtime_dir(PSX_RUNTIME_DIR_BIN));
    env_add(out, max, &written, "PATH", path);

    env_add(out, max, &written, "SSL_CERT_FILE", g_ca_bundle);
    env_add(out, max, &written, "CURL_CA_BUNDLE", g_ca_bundle);
    env_add(out, max, &written, "TERM", "xterm-256color");

    if(written < max) {
        out[written] = NULL;
    }

    return written;
}

bool
psx_runtime_manifest_write(const char *extra_packages_json)
{
    FILE *file;

    runtime_init();

    if(!(file = fopen(g_manifest, "w"))) {
        PSX_LOGW("runtime: cannot write %s: %s", g_manifest,
                 strerror(errno));
        return false;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"version\": 1,\n");
    fprintf(file, "  \"platform\": \"%s\",\n", psx_platform_id());
    fprintf(file, "  \"arch\": \"x86_64\",\n");
    fprintf(file, "  \"root\": \"%s\",\n", g_root);
    fprintf(file, "  \"packages\": %s\n",
            extra_packages_json && *extra_packages_json ? extra_packages_json
                                                        : "{}");
    fprintf(file, "}\n");

    fclose(file);

    return true;
}
