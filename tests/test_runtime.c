/*
 * Runtime layout: paths, directories and the environment handed to external
 * CLI processes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "psx_test.h"
#include "psxterm/platform.h"
#include "psxterm/runtime.h"

static void
test_paths(void)
{
    const char *root = psx_runtime_root();

    PSX_CHECK(root != NULL);
    PSX_CHECK(strlen(root) > 0);
    PSX_CHECK(strstr(root, "/runtime") != NULL);

    /* The layout the roadmap asks for, spelled out once. */
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_BIN), "/runtime/bin"));
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_LIB), "/runtime/lib"));
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_ETC), "/runtime/etc"));
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_TMP), "/runtime/tmp"));
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_HOME), "/runtime/home"));
    PSX_CHECK(
        strstr(psx_runtime_dir(PSX_RUNTIME_DIR_CACHE), "/runtime/cache"));

    /* Agents and workspaces sit beside the runtime, under the home root. */
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_AGENTS), "agents"));
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_AGENTS), "/runtime") ==
              NULL);
    PSX_CHECK(strstr(psx_runtime_dir(PSX_RUNTIME_DIR_WORKSPACES),
                     "workspaces"));

    PSX_CHECK(strstr(psx_runtime_ca_bundle(), "/runtime/etc/ca-bundle.crt"));
    PSX_CHECK(strstr(psx_runtime_manifest_path(), "/runtime/runtime.json"));
}

static void
test_environment(void)
{
    char *env[16];
    size_t count = psx_runtime_environment(env, 16);
    bool has_home = false;
    bool has_tmpdir = false;
    bool has_xdg = false;
    bool has_path = false;
    bool has_tls = false;
    bool has_term = false;

    PSX_CHECK(count >= 9);

    for(size_t i = 0; i < count; i++) {
        PSX_CHECK(env[i] != NULL);

        if(strncmp(env[i], "HOME=/", 6) == 0) {
            has_home = true;
        }
        if(strncmp(env[i], "TMPDIR=/", 8) == 0) {
            has_tmpdir = true;
        }
        if(strncmp(env[i], "XDG_CONFIG_HOME=/", 17) == 0 ||
           strncmp(env[i], "XDG_CACHE_HOME=/", 16) == 0 ||
           strncmp(env[i], "XDG_DATA_HOME=/", 15) == 0) {
            has_xdg = true;
        }
        if(strncmp(env[i], "PATH=", 5) == 0) {
            has_path = strstr(env[i], "/runtime/bin") != NULL;
        }
        if(strncmp(env[i], "SSL_CERT_FILE=", 14) == 0) {
            has_tls = strstr(env[i], "ca-bundle.crt") != NULL;
        }
        if(strncmp(env[i], "TERM=", 5) == 0) {
            has_term = true;
        }

        free(env[i]);
    }

    PSX_CHECK(has_home);
    PSX_CHECK(has_tmpdir);
    PSX_CHECK(has_xdg);
    PSX_CHECK(has_path);
    PSX_CHECK(has_tls);
    PSX_CHECK(has_term);

    /* Bounded writes must still terminate the vector. */
    {
        char *small[4];

        memset(small, 0, sizeof(small));
        PSX_CHECK(psx_runtime_environment(small, 3) == 3);
        PSX_CHECK(small[3] == NULL);
        for(size_t i = 0; i < 3; i++) {
            free(small[i]);
        }
    }
}

static void
test_prepare(void)
{
    struct stat st;

    PSX_CHECK(psx_runtime_prepare());
    /* Idempotent by contract. */
    PSX_CHECK(psx_runtime_prepare());

    PSX_CHECK(stat(psx_runtime_dir(PSX_RUNTIME_DIR_BIN), &st) == 0);
    PSX_CHECK(S_ISDIR(st.st_mode));
    PSX_CHECK(stat(psx_runtime_dir(PSX_RUNTIME_DIR_TMP), &st) == 0);
    PSX_CHECK(stat(psx_runtime_dir(PSX_RUNTIME_DIR_HOME), &st) == 0);
    PSX_CHECK(stat(psx_runtime_dir(PSX_RUNTIME_DIR_AGENTS), &st) == 0);
    PSX_CHECK(stat(psx_runtime_dir(PSX_RUNTIME_DIR_WORKSPACES), &st) == 0);

    {
        char nested[512];

        snprintf(nested, sizeof(nested), "%s/.config",
                 psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
        PSX_CHECK(stat(nested, &st) == 0);
        PSX_CHECK(strlen(nested) < sizeof(nested));
    }
}

static void
test_manifest(void)
{
    FILE *file;
    char line[512];
    bool has_version = false;
    bool has_platform = false;

    PSX_CHECK(psx_runtime_manifest_write("{\"curl\": \"8.0\"}"));

    if((file = fopen(psx_runtime_manifest_path(), "r"))) {
        while(fgets(line, sizeof(line), file)) {
            if(strstr(line, "\"version\": 1")) {
                has_version = true;
            }
            if(strstr(line, "\"platform\"")) {
                has_platform = true;
            }
        }
        fclose(file);
    }

    PSX_CHECK(has_version);
    PSX_CHECK(has_platform);
}

int
main(void)
{
    /* Keep the layout test inside a throwaway base, never the real home. */
    setenv("PSXTERM_RUNTIME_BASE", "/tmp/psxterm-runtime-test", 1);

    test_paths();
    test_environment();
    test_prepare();
    test_manifest();

    return PSX_TEST_SUMMARY();
}
