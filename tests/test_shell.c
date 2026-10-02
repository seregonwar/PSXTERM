#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "psxterm/protocol.h"
#include "psxterm/session.h"
#include "psxterm/shell.h"
#include "psxterm/util.h"

#include "psx_test.h"

typedef struct {
    psx_session_t *session;
    psx_shell_t *shell;
    int peer_fd;
    ptty_reader_t reader;
} fixture_t;

static void
fixture_open(fixture_t *fx)
{
    int fds[2];

    PSX_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    PSX_CHECK_EQ(psx_set_nonblocking(fds[1], true), 0);

    fx->session = psx_session_create(1, fds[0]);
    fx->peer_fd = fds[1];
    ptty_reader_init(&fx->reader, fx->peer_fd);

    psx_env_init(&fx->session->env, NULL);
    snprintf(fx->session->cwd, sizeof(fx->session->cwd), "%s", "/tmp");
    fx->shell = psh_shell_create(fx->session);
    fx->session->shell = fx->shell;
    fx->session->state = PSX_SESSION_RUNNING;
}

static void
fixture_close(fixture_t *fx)
{
    ptty_reader_destroy(&fx->reader);
    psx_session_destroy(fx->session);
    close(fx->peer_fd);
}

/* Collect all currently available STDOUT/STDERR payloads. */
static size_t
collect(fixture_t *fx, char *out, size_t cap)
{
    size_t used = 0;

    out[0] = '\0';

    for(;;) {
        ptty_header_t header;
        const uint8_t *payload = NULL;
        ptty_read_result_t rc = ptty_read_frame(&fx->reader, &header, &payload);

        if(rc != PTTY_READ_OK) {
            break;
        }

        if((header.type == PTTY_MSG_STDOUT ||
            header.type == PTTY_MSG_STDERR) &&
           payload) {
            size_t n = header.payload_length;

            if(used + n + 1 > cap) {
                n = cap > used + 1 ? cap - used - 1 : 0;
            }
            memcpy(out + used, payload, n);
            used += n;
            out[used] = '\0';
        }
    }

    return used;
}

static void
run_line(fixture_t *fx, const char *line, char *out, size_t cap)
{
    char with_newline[PSH_LINE_MAX];

    snprintf(with_newline, sizeof(with_newline), "%s\n", line);
    psh_shell_feed(fx->shell, (const uint8_t *)with_newline,
                   strlen(with_newline));
    psx_session_flush(fx->session);
    collect(fx, out, cap);
}

static void
expect_contains(const char *haystack, const char *needle, const char *context)
{
    PSX_CHECK_MSG(strstr(haystack, needle) != NULL,
                  "%s: '%s' not found in output:\n%s", context, needle,
                  haystack);
}

static void
test_builtins(void)
{
    fixture_t fx;
    char out[8192];

    fixture_open(&fx);

    run_line(&fx, "pwd", out, sizeof(out));
    expect_contains(out, "/tmp", "pwd");

    run_line(&fx, "help", out, sizeof(out));
    expect_contains(out, "pwd", "help");
    expect_contains(out, "available commands", "help");

    run_line(&fx, "export GREETING=hello", out, sizeof(out));
    run_line(&fx, "env", out, sizeof(out));
    expect_contains(out, "GREETING=hello", "env after export");

    run_line(&fx, "unset GREETING", out, sizeof(out));
    run_line(&fx, "env", out, sizeof(out));
    PSX_CHECK_MSG(strstr(out, "GREETING=hello") == NULL,
                  "unset did not remove the variable");

    run_line(&fx, "cd /", out, sizeof(out));
    run_line(&fx, "pwd", out, sizeof(out));
    expect_contains(out, "/\n", "pwd after cd /");

    run_line(&fx, "cat", out, sizeof(out));
    expect_contains(out, "missing file operand", "cat without arguments");

    run_line(&fx, "no_such_command_xyz", out, sizeof(out));
    expect_contains(out, "command not found", "unknown command");

    run_line(&fx, "uname", out, sizeof(out));
    PSX_CHECK(out[0] != '\0');

    fixture_close(&fx);
}

static void
test_quoting_and_cat(void)
{
    fixture_t fx;
    char out[8192];
    const char *name = "/tmp/psxterm shell test.txt";
    int fd;

    if((fd = open(name, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0) {
        PSX_CHECK_MSG(0, "cannot create %s", name);
        return;
    }
    if(write(fd, "quoted content\n", 15) != 15) {
        PSX_CHECK(0);
    }
    close(fd);

    fixture_open(&fx);

    run_line(&fx, "cat \"/tmp/psxterm shell test.txt\"", out, sizeof(out));
    expect_contains(out, "quoted content", "cat with a quoted path");

    fixture_close(&fx);
    unlink(name);
}

static void
test_line_editing(void)
{
    fixture_t fx;
    char out[8192];

    fixture_open(&fx);

    /* Backspace: "pwx" + DEL + "d" runs pwd. */
    {
        const uint8_t input[] = "pwx\x7f" "d\n";

        psh_shell_feed(fx.shell, input, sizeof(input) - 1);
        psx_session_flush(fx.session);
        collect(&fx, out, sizeof(out));
        expect_contains(out, "/tmp", "backspace editing");
        PSX_CHECK_MSG(strstr(out, "command not found") == NULL,
                      "backspace produced a bad command line");
    }

    /* Ctrl+C cancels the line. */
    {
        const uint8_t input[] = "garbage\x03";

        psh_shell_feed(fx.shell, input, sizeof(input) - 1);
        psx_session_flush(fx.session);
        collect(&fx, out, sizeof(out));
        expect_contains(out, "^C", "ctrl+c echo");
    }

    run_line(&fx, "pwd", out, sizeof(out));
    expect_contains(out, "/tmp", "shell still usable after ctrl+c");

    fixture_close(&fx);
}

static void
test_exit(void)
{
    fixture_t fx;
    char out[8192];

    fixture_open(&fx);

    run_line(&fx, "exit 3", out, sizeof(out));
    PSX_CHECK(psh_shell_exit_requested(fx.shell));
    PSX_CHECK_EQ(psh_shell_exit_code(fx.shell), 3);

    fixture_close(&fx);

    /* Ctrl+D on an empty line exits with the last status. */
    fixture_open(&fx);
    {
        const uint8_t eof = 0x04;

        psh_shell_feed(fx.shell, &eof, 1);
        PSX_CHECK(psh_shell_exit_requested(fx.shell));
        PSX_CHECK_EQ(psh_shell_exit_code(fx.shell), 0);
    }
    fixture_close(&fx);
}

static void
test_exec_line_api(void)
{
    fixture_t fx;
    char out[8192];

    fixture_open(&fx);

    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "pwd"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    expect_contains(out, "/tmp", "execute_line(pwd)");

    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "exit 0"), 0);
    PSX_CHECK(psh_shell_exit_requested(fx.shell));

    fixture_close(&fx);
}

int
main(void)
{
    test_builtins();
    test_quoting_and_cat();
    test_line_editing();
    test_exit();
    test_exec_line_api();

    return PSX_TEST_SUMMARY();
}
