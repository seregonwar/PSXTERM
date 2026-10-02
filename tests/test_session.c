#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psxterm/session.h"

#include "psx_test.h"

static psx_session_t *
make_session(const char *cwd)
{
    int fds[2];
    psx_session_t *session;

    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    session = psx_session_create(1, fds[0]);
    PSX_CHECK(session != NULL);
    close(fds[1]);

    psx_env_init(&session->env, NULL);
    psx_env_set(&session->env, "HOME", "/home/tester");
    snprintf(session->cwd, sizeof(session->cwd), "%s", cwd);

    return session;
}

static void
check_abs(const char *cwd, const char *path, const char *expected)
{
    psx_session_t *session = make_session(cwd);
    char out[PSX_PATH_MAX];

    PSX_CHECK_EQ(psx_session_absolute_path(session, path, out, sizeof(out)), 0);
    PSX_CHECK_MSG(strcmp(out, expected) == 0, "%s + %s -> %s (want %s)", cwd,
                  path, out, expected);

    psx_session_destroy(session);
}

static void
test_absolute_paths(void)
{
    check_abs("/home/tester/work", "file.txt", "/home/tester/work/file.txt");
    check_abs("/home/tester/work", ".", "/home/tester/work");
    check_abs("/home/tester/work", "..", "/home/tester");
    check_abs("/home/tester/work", "../docs/a.txt", "/home/tester/docs/a.txt");
    check_abs("/home/tester", "a/./b/../c", "/home/tester/a/c");
    check_abs("/", "etc/passwd", "/etc/passwd");
    check_abs("/home/tester", "/absolute/path", "/absolute/path");
    check_abs("/a/b", "//x///y", "/x/y");
    check_abs("/a/b", "../../..", "/");
    check_abs("/home/tester", "~/notes", "/home/tester/notes");
    check_abs("/home/tester", "~", "/home/tester");
}

static void
test_path_overflow(void)
{
    psx_session_t *session = make_session("/tmp");
    char out[16];

    PSX_CHECK_EQ(psx_session_absolute_path(session, "a/very/long/relative/path",
                                            out, sizeof(out)),
                 -1);
    PSX_CHECK_EQ(errno, ENAMETOOLONG);

    {
        char too_long[PSX_PATH_MAX + 16];

        memset(too_long, 'a', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        PSX_CHECK_EQ(psx_session_absolute_path(session, too_long, out,
                                               sizeof(out)),
                     -1);
    }

    psx_session_destroy(session);
}

static void
test_resolve_path(void)
{
    char dir[] = "/tmp/psxterm-test-XXXXXX";
    char tool[PSX_PATH_MAX];
    char tool_elf[PSX_PATH_MAX];
    psx_session_t *session;
    char *resolved;
    int fd;

    PSX_CHECK(mkdtemp(dir) != NULL);

    snprintf(tool, sizeof(tool), "%s/tool", dir);
    snprintf(tool_elf, sizeof(tool_elf), "%s/tool.elf", dir);

    if((fd = open(tool, O_CREAT | O_WRONLY, 0644)) >= 0) {
        close(fd);
    }
    if((fd = open(tool_elf, O_CREAT | O_WRONLY, 0644)) >= 0) {
        close(fd);
    }

    session = make_session(dir);

    /* explicit path */
    resolved = psx_session_resolve_path(session, tool);
    PSX_CHECK(resolved != NULL);
    if(resolved) {
        PSX_CHECK_STR_EQ(resolved, tool);
        free(resolved);
    }

    /* PATH lookup without extension */
    PSX_CHECK_EQ(psx_env_set(&session->env, "PATH", dir), 0);
    resolved = psx_session_resolve_path(session, "tool");
    PSX_CHECK(resolved != NULL);
    if(resolved) {
        PSX_CHECK_STR_EQ(resolved, tool);
        free(resolved);
    }

    /* .elf fallback */
    resolved = psx_session_resolve_path(session, "tool.elf");
    PSX_CHECK(resolved != NULL);
    if(resolved) {
        PSX_CHECK_STR_EQ(resolved, tool_elf);
        free(resolved);
    }

    /* not found */
    PSX_CHECK(psx_session_resolve_path(session, "missing-tool") == NULL);

    psx_session_destroy(session);
    unlink(tool);
    unlink(tool_elf);
    rmdir(dir);
}

static void
test_manager(void)
{
    psx_session_manager_t manager;
    int fds_a[2];
    int fds_b[2];
    int fds_c[2];
    psx_session_t *a;
    psx_session_t *b;

    psx_session_manager_init(&manager, 2);

    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_a), 0);
    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_b), 0);
    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_c), 0);

    a = psx_session_manager_create(&manager, fds_a[0]);
    b = psx_session_manager_create(&manager, fds_b[0]);
    PSX_CHECK(a != NULL);
    PSX_CHECK(b != NULL);
    PSX_CHECK_EQ(psx_session_manager_count(&manager), 2);
    PSX_CHECK(a->id != b->id);

    /* limit reached */
    PSX_CHECK(psx_session_manager_create(&manager, fds_c[0]) == NULL);

    PSX_CHECK(psx_session_manager_find(&manager, a->id) == a);
    if(psx_session_manager_find(&manager, a->id) == a) {
        uint32_t id = a->id;

        psx_session_manager_remove(&manager, a);
        PSX_CHECK_EQ(psx_session_manager_count(&manager), 1);
        PSX_CHECK(psx_session_manager_find(&manager, id) == NULL);
        PSX_CHECK(psx_session_manager_find(&manager, b->id) == b);
    }

    psx_session_manager_remove(&manager, b);

    close(fds_a[1]);
    close(fds_b[1]);
    close(fds_c[1]);
    close(fds_c[0]);
}

int
main(void)
{
    test_absolute_paths();
    test_path_overflow();
    test_resolve_path();
    test_manager();

    return PSX_TEST_SUMMARY();
}
