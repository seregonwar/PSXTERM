#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/*
 * A listing has to look like one from a Linux terminal: directories coloured
 * and names side by side, inside the width the session reports.
 */
static void
test_ls_listing(void)
{
    char dir[PSX_PATH_MAX];
    char line[PSX_PATH_MAX + 32];
    char out[8192];
    fixture_t fx;
    int fd;

    snprintf(dir, sizeof(dir), "/tmp/psxterm-ls-%d", (int)getpid());
    mkdir(dir, 0755);

    for(int i = 0; i < 6; i++) {
        char path[PSX_PATH_MAX + 32];

        snprintf(path, sizeof(path), "%s/file-%d.txt", dir, i);
        if((fd = open(path, O_CREAT | O_WRONLY, 0644)) >= 0) {
            close(fd);
        }
    }

    {
        char path[PSX_PATH_MAX + 32];

        snprintf(path, sizeof(path), "%s/subdir", dir);
        mkdir(path, 0755);

        snprintf(path, sizeof(path), "%s/runner", dir);
        if((fd = open(path, O_CREAT | O_WRONLY, 0755)) >= 0) {
            close(fd);
            chmod(path, 0755);
        }
    }

    fixture_open(&fx);
    fx.session->cols = 80;

    snprintf(line, sizeof(line), "ls %s", dir);
    run_line(&fx, line, out, sizeof(out));

    /* A directory is drawn in the blue every terminal uses for one. */
    expect_contains(out, "\033[1;94msubdir\033[0m", "directory colour");

    /* An executable is green, and a plain file is not coloured at all. */
    expect_contains(out, "\033[1;92mrunner\033[0m", "executable colour");
    expect_contains(out, "file-0.txt", "plain file present");
    PSX_CHECK_MSG(strstr(out, "\033[1;92mfile-0.txt") == NULL,
                  "a plain file must not be drawn as an executable");

    /* Names share a line instead of one per line, and none overflows the
     * width. */
    {
        size_t longest = 0;
        size_t current = 0;
        int escapes = 0;

        for(const char *p = out; *p; p++) {
            if(*p == '\033') {
                escapes++;
                continue;
            }

            if(escapes) {
                if(*p == 'm') {
                    escapes = 0;
                }
                continue;
            }

            if(*p == '\n') {
                if(current > longest) {
                    longest = current;
                }
                current = 0;
            } else {
                current++;
            }
        }

        PSX_CHECK_MSG(longest > 0 && longest <= (size_t)fx.session->cols,
                      "a line of %zu columns does not fit in %u", longest,
                      fx.session->cols);

        /*
         * Two names share a line. The layout fills down the columns and then
         * across, which is what the GNU tool does, so the neighbour on the same
         * line is the one a whole column away.
         */
        {
            const char *first = strstr(out, "file-0.txt");
            const char *neighbour = first ? strstr(first, "file-2.txt") : NULL;
            const char *newline = first ? strchr(first, '\n') : NULL;

            PSX_CHECK_MSG(neighbour && newline && neighbour < newline,
                          "names are not laid out side by side:\n%s", out);
        }
    }

    /* -l is one entry per line with the mode in front. */
    snprintf(line, sizeof(line), "ls -l %s", dir);
    run_line(&fx, line, out, sizeof(out));
    expect_contains(out, "drwx", "long format shows the directory mode");
    expect_contains(out, "-rwx", "long format shows the executable mode");
    expect_contains(out, "total ", "long format prints a total");

    /* -1 is one name per line, and --color=never leaves the escapes out. */
    snprintf(line, sizeof(line), "ls -1 --color=never %s", dir);
    run_line(&fx, line, out, sizeof(out));
    expect_contains(out, "subdir\n", "-1 puts one name per line");
    PSX_CHECK_MSG(strstr(out, "\033[") == NULL,
                  "--color=never still emitted an escape:\n%s", out);

    /* -F appends the class suffix, as the GNU tool does. */
    snprintf(line, sizeof(line), "ls -1F --color=never %s", dir);
    run_line(&fx, line, out, sizeof(out));
    expect_contains(out, "subdir/", "-F marks a directory");
    expect_contains(out, "runner*", "-F marks an executable");

    /* A narrow session must fold the columns rather than wrap them. */
    fx.session->cols = 24;
    snprintf(line, sizeof(line), "ls --color=never %s", dir);
    run_line(&fx, line, out, sizeof(out));
    PSX_CHECK_MSG(strstr(out, "file-0.txt file-1.txt") == NULL,
                  "columns ignored a narrow session:\n%s", out);

    /* LS_COLORS overrides the defaults, as it does everywhere else. */
    psx_env_set(&fx.session->env, "LS_COLORS", "di=1;31");
    fx.session->cols = 80;
    snprintf(line, sizeof(line), "ls %s", dir);
    run_line(&fx, line, out, sizeof(out));
    expect_contains(out, "\033[1;31msubdir\033[0m", "LS_COLORS override");

    fixture_close(&fx);

    {
        char path[PSX_PATH_MAX + 32];

        for(int i = 0; i < 6; i++) {
            snprintf(path, sizeof(path), "%s/file-%d.txt", dir, i);
            unlink(path);
        }

        snprintf(path, sizeof(path), "%s/subdir", dir);
        rmdir(path);
        snprintf(path, sizeof(path), "%s/runner", dir);
        unlink(path);
        rmdir(dir);
    }
}

static void
test_printable_names(void)
{
    static const struct {
        const char *name;
        size_t width;
    } cases[] = {
        { "plain", 5 }, { "caff\xc3\xa8", 5 },
        { "\xe4\xb8\xad\xe6\x96\x87", 4 },
        { "e\xcc\x81", 1 }, { "\xf0\x9f\x93\x81", 2 },
        { "bad\x1b", 7 }, { "\xc3", 4 }, { "\xf0\x9f", 8 },
        { "\xc0\xaf", 8 }, { "\xed\xa0\x80", 12 },
        { "\xf4\x90\x80\x80", 16 }, { "\xc2\x9b", 8 },
        { "\xe2\x80\xae", 12 }
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t width = 0;
        char *text = psh_display_text(cases[i].name, false, &width);
        PSX_CHECK(text != NULL);
        PSX_CHECK_EQ(width, cases[i].width);
        if(text) {
            PSX_CHECK(strchr(text, '\033') == NULL);
            free(text);
        }
    }
    char *text = psh_display_text("two words", true, NULL);
    PSX_CHECK_STR_EQ(text, "\"two words\"");
    free(text);
    text = psh_display_text("a'quote", true, NULL);
    PSX_CHECK_STR_EQ(text, "\"a'quote\"");
    free(text);
}

static void
test_long_outputs_and_cwd(void)
{
    fixture_t fx;
    char out[20000];
    char value[10001];
    memset(value, 'x', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    fixture_open(&fx);
    PSX_CHECK_EQ(psx_env_set(&fx.session->env, "LONG_VALUE", value), 0);
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "env"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    char *entry = strstr(out, "LONG_VALUE=");
    PSX_CHECK(entry != NULL);
    if(entry) {
        PSX_CHECK_EQ(strcspn(entry + 11, "\n"), 10000);
    }
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "cd /"), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&fx.session->env, "OLDPWD"), "/tmp");
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "cd -"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    PSX_CHECK_STR_EQ(out, "/tmp\n");
    PSX_CHECK_STR_EQ(fx.session->cwd, "/tmp");
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "ls -ldn --color=never"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    PSX_CHECK(strncmp(out, "drwx", 4) == 0);
    PSX_CHECK(strstr(out, " .\n") != NULL);
    psx_env_set(&fx.session->env, "TERM", "dumb");
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "ls -d"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    PSX_CHECK_STR_EQ(out, ".\n");
    fixture_close(&fx);
}

static void
test_session_table_output(void)
{
    fixture_t fx;
    char out[8192];
    psx_session_manager_t manager = { 0 };
    psx_session_t other = { 0 };
    fixture_open(&fx);
    manager.sessions = &other;
    other.next = fx.session;
    other.id = UINT32_MAX;
    other.state = PSX_SESSION_DETACHED;
    other.proc.pid = 123456789;
    snprintf(other.client_name, sizeof(other.client_name), "name\033[31m\n");
    fx.session->manager = &manager;
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "ps"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    PSX_CHECK(strstr(out, "4294967295  detached") != NULL);
    PSX_CHECK(strstr(out, "123456789") != NULL);
    PSX_CHECK(strstr(out, "name\\x1b[31m\\x0a") != NULL);
    PSX_CHECK(strchr(out, '\033') == NULL);
    char *header = strstr(out, "ID  STATE");
    char *row = strstr(out, "4294967295");
    if(header && row) {
        /* Header begins at the right edge of the ID column. */
        PSX_CHECK_EQ(strstr(header, "STATE") - header,
                     strstr(row, "detached") - row - 8);
    }
    fx.session->manager = NULL;
    PSX_CHECK_EQ(psx_env_set(&fx.session->env, "USER", "user\033[31m"), 0);
    PSX_CHECK_EQ(psh_shell_execute_line(fx.shell, "whoami"), 0);
    psx_session_flush(fx.session);
    collect(&fx, out, sizeof(out));
    PSX_CHECK_STR_EQ(out, "user\\x1b[31m\n");
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
    test_ls_listing();
    test_printable_names();
    test_long_outputs_and_cwd();
    test_session_table_output();

    return PSX_TEST_SUMMARY();
}
