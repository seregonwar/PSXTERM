/*
 * Runtime layout: paths, directories and the environment handed to external
 * CLI processes.
 */

#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

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
            /* A search list: runtime bin first, no doubled separator. */
            has_path = strstr(env[i], "/runtime/bin:") != NULL &&
                       strstr(env[i], "//") == NULL;
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
    PSX_CHECK(env[count] == NULL);

    /* Bounded writes must still terminate the vector. */
    {
        char *small[4];

        for(size_t i = 0; i < 4; i++) {
            small[i] = (char *)"untouched";
        }
        PSX_CHECK(psx_runtime_environment(small, 4) == 3);
        PSX_CHECK(small[3] == NULL);
        for(size_t i = 0; i < 3; i++) {
            free(small[i]);
        }
    }

    {
        char *one[2] = {(char *)"untouched", (char *)"sentinel"};

        PSX_CHECK(psx_runtime_environment(one, 1) == 0);
        PSX_CHECK(one[0] == NULL);
        PSX_CHECK_STR_EQ(one[1], "sentinel");
        PSX_CHECK(psx_runtime_environment(NULL, 0) == 0);
        errno = 0;
        PSX_CHECK(psx_runtime_environment(NULL, 1) == 0);
        PSX_CHECK_EQ(errno, EINVAL);
    }

    /* Every boundary must terminate within capacity and leave a guard alone. */
    for(size_t capacity = 1; capacity <= 11; capacity++) {
        char *bounded[12];
        size_t written;

        for(size_t i = 0; i < 12; i++) {
            bounded[i] = (char *)"guard";
        }
        written = psx_runtime_environment(bounded, capacity);
        PSX_CHECK_EQ(written, capacity <= 9 ? capacity - 1 : 9);
        PSX_CHECK(bounded[written] == NULL);
        PSX_CHECK_STR_EQ(bounded[capacity], "guard");
        for(size_t i = 0; i < written; i++) {
            free(bounded[i]);
        }
    }
}

static void
test_prepare_rejects_files(void)
{
    const char *root = psx_runtime_root();
    FILE *file = fopen(root, "w");

    PSX_CHECK(file != NULL);
    if(file) {
        fclose(file);
        PSX_CHECK(!psx_runtime_prepare());
        PSX_CHECK_EQ(errno, ENOTDIR);
        PSX_CHECK(unlink(root) == 0);
    }

    PSX_CHECK(mkdir(root, 0755) == 0);
    file = fopen(psx_runtime_dir(PSX_RUNTIME_DIR_BIN), "w");
    PSX_CHECK(file != NULL);
    if(file) {
        fclose(file);
        PSX_CHECK(!psx_runtime_prepare());
        PSX_CHECK_EQ(errno, ENOTDIR);
        PSX_CHECK(unlink(psx_runtime_dir(PSX_RUNTIME_DIR_BIN)) == 0);
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

    PSX_CHECK(psx_runtime_manifest_write(NULL));
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

    /* Startup must retain the installed package metadata byte for byte. */
    {
        char before[2048] = {0};
        char after[2048] = {0};
        size_t before_len = 0;
        size_t after_len = 0;

        file = fopen(psx_runtime_manifest_path(), "r");
        PSX_CHECK(file != NULL);
        if(file) {
            before_len = fread(before, 1, sizeof(before) - 1, file);
            fclose(file);
        }
        PSX_CHECK(psx_runtime_manifest_write(NULL));
        file = fopen(psx_runtime_manifest_path(), "r");
        PSX_CHECK(file != NULL);
        if(file) {
            after_len = fread(after, 1, sizeof(after) - 1, file);
            fclose(file);
        }
        PSX_CHECK_EQ(before_len, after_len);
        PSX_CHECK_STR_EQ(before, after);

        /* A failed flush must neither replace metadata nor leave staging files.
         * The file-size limit is confined to a child to keep other tests safe. */
        {
            pid_t pid = fork();
            int status = 0;

            PSX_CHECK(pid >= 0);
            if(pid == 0) {
                struct rlimit limit = {0, 0};
                bool failed;

                signal(SIGXFSZ, SIG_IGN);
                if(setrlimit(RLIMIT_FSIZE, &limit) != 0) {
                    _exit(2);
                }
                failed = !psx_runtime_manifest_write("{\"curl\": \"new\"}");
                _exit(failed && errno == EFBIG ? 0 : 1);
            }
            if(pid > 0) {
                PSX_CHECK(waitpid(pid, &status, 0) == pid);
                PSX_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
        }

        memset(after, 0, sizeof(after));
        file = fopen(psx_runtime_manifest_path(), "r");
        PSX_CHECK(file != NULL);
        if(file) {
            after_len = fread(after, 1, sizeof(after) - 1, file);
            fclose(file);
        }
        PSX_CHECK_EQ(before_len, after_len);
        PSX_CHECK_STR_EQ(before, after);

        {
            DIR *dir = opendir(psx_runtime_root());
            struct dirent *entry;

            PSX_CHECK(dir != NULL);
            if(dir) {
                while((entry = readdir(dir))) {
                    PSX_CHECK(strncmp(entry->d_name, ".runtime.json-", 14) != 0);
                }
                closedir(dir);
            }
        }
    }
}

int
main(void)
{
    /* Keep the layout test inside a throwaway base, never the real home. */
    char base[] = "/tmp/psxterm-test-XXXXXX";
    char nested[512];

    if(!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("PSXTERM_RUNTIME_BASE", base, 1);

    test_paths();
    test_environment();
    test_prepare_rejects_files();
    test_prepare();
    test_manifest();

    PSX_CHECK(unlink(psx_runtime_manifest_path()) == 0);
    snprintf(nested, sizeof(nested), "%s/.config",
             psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
    PSX_CHECK(rmdir(nested) == 0);
    snprintf(nested, sizeof(nested), "%s/.local/share",
             psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
    PSX_CHECK(rmdir(nested) == 0);
    snprintf(nested, sizeof(nested), "%s/.local",
             psx_runtime_dir(PSX_RUNTIME_DIR_HOME));
    PSX_CHECK(rmdir(nested) == 0);
    for(size_t i = 0; i < PSX_RUNTIME_DIR_COUNT; i++) {
        PSX_CHECK(rmdir(psx_runtime_dir((psx_runtime_dir_t)i)) == 0);
    }
    PSX_CHECK(rmdir(psx_runtime_root()) == 0);
    PSX_CHECK(rmdir(base) == 0);

    return PSX_TEST_SUMMARY();
}
