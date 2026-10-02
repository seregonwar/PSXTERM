#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psxterm/diag.h"
#include "psxterm/process.h"

#include "psx_test.h"

static void
test_report_bookkeeping(void)
{
    psx_diag_report_t report;
    psx_diag_group_t *platform;
    psx_diag_group_t *tty;
    psx_diag_group_t *again;

    psx_diag_report_init(&report);
    PSX_CHECK_EQ(report.count, 0);
    PSX_CHECK_EQ(report.groups, NULL);

    platform = psx_diag_group(&report, "Platform");
    tty = psx_diag_group(&report, "TTY");
    again = psx_diag_group(&report, "Platform");

    PSX_CHECK(platform != NULL);
    PSX_CHECK(tty != NULL);
    PSX_CHECK(platform == again);
    PSX_CHECK_EQ(report.count, 2);
    PSX_CHECK_STR_EQ(report.groups[0].name, "Platform");
    PSX_CHECK_STR_EQ(report.groups[1].name, "TTY");

    psx_diag_add(platform, "identity", PSX_DIAG_PASS, 0, "value %d", 42);
    PSX_CHECK_EQ(platform->count, 1);
    PSX_CHECK_STR_EQ(platform->checks[0].name, "identity");
    PSX_CHECK_STR_EQ(platform->checks[0].detail, "value 42");
    PSX_CHECK_EQ(platform->checks[0].status, PSX_DIAG_PASS);
    PSX_CHECK_EQ(platform->checks[0].error_code, 0);

    psx_diag_add(platform, "without detail", PSX_DIAG_SKIP, 7, NULL);
    PSX_CHECK_EQ(platform->checks[1].error_code, 7);
    PSX_CHECK_STR_EQ(platform->checks[1].detail, "");

    /* NULL group must be tolerated */
    psx_diag_add(NULL, "ignored", PSX_DIAG_PASS, 0, "x");

    psx_diag_report_free(&report);
    PSX_CHECK_EQ(report.count, 0);
    PSX_CHECK_EQ(report.groups, NULL);
}

static psx_diag_report_t
make_report(psx_diag_status_t first, psx_diag_status_t second)
{
    psx_diag_report_t report;

    psx_diag_report_init(&report);
    psx_diag_add(psx_diag_group(&report, "G"), "one", first, 0, "a");
    psx_diag_add(psx_diag_group(&report, "G"), "two", second, 0, "b");

    return report;
}

static void
test_overall_status(void)
{
    psx_diag_report_t report;

    report = make_report(PSX_DIAG_PASS, PSX_DIAG_PASS);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_PASS);
    PSX_CHECK_STR_EQ(psx_diag_overall_name(PSX_DIAG_PASS), "READY");
    PSX_CHECK_EQ(psx_diag_exit_code(PSX_DIAG_PASS), PSX_DIAG_EXIT_READY);
    psx_diag_report_free(&report);

    report = make_report(PSX_DIAG_PASS, PSX_DIAG_WARN);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_WARN);
    PSX_CHECK_EQ(psx_diag_exit_code(PSX_DIAG_WARN), PSX_DIAG_EXIT_WARNINGS);
    psx_diag_report_free(&report);

    report = make_report(PSX_DIAG_WARN, PSX_DIAG_FAIL);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_FAIL);
    PSX_CHECK_STR_EQ(psx_diag_overall_name(PSX_DIAG_FAIL), "NOT READY");
    PSX_CHECK_EQ(psx_diag_exit_code(PSX_DIAG_FAIL), PSX_DIAG_EXIT_FAILED);
    psx_diag_report_free(&report);

    /* SKIP and UNKNOWN are informational, not degradations */
    report = make_report(PSX_DIAG_SKIP, PSX_DIAG_UNKNOWN);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_PASS);
    psx_diag_report_free(&report);

    report = make_report(PSX_DIAG_UNKNOWN, PSX_DIAG_WARN);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_WARN);
    psx_diag_report_free(&report);

    psx_diag_report_init(&report);
    PSX_CHECK_EQ(psx_diag_overall_status(&report), PSX_DIAG_PASS);
    psx_diag_report_free(&report);
}

static void
test_status_names(void)
{
    PSX_CHECK_STR_EQ(psx_diag_status_name(PSX_DIAG_PASS), "PASS");
    PSX_CHECK_STR_EQ(psx_diag_status_name(PSX_DIAG_FAIL), "FAIL");
    PSX_CHECK_STR_EQ(psx_diag_status_name(PSX_DIAG_WARN), "WARN");
    PSX_CHECK_STR_EQ(psx_diag_status_name(PSX_DIAG_SKIP), "SKIP");
    PSX_CHECK_STR_EQ(psx_diag_status_name(PSX_DIAG_UNKNOWN), "UNKNOWN");
}

static void
test_renderings(void)
{
    psx_diag_report_t report;
    char *text;
    size_t needed;
    size_t small_len;

    psx_diag_report_init(&report);
    psx_diag_add(psx_diag_group(&report, "Group A"), "ok", PSX_DIAG_PASS, 0,
                 "fine");
    psx_diag_add(psx_diag_group(&report, "Group A"), "bad", PSX_DIAG_WARN, 13,
                 "quote \" and backslash \\ and newline \n");
    report.elapsed_ms = 12;

    needed = psx_diag_format_human(&report, NULL, 0);
    PSX_CHECK(needed > 0);

    if((text = malloc(needed + 1))) {
        PSX_CHECK_EQ(psx_diag_format_human(&report, text, needed + 1), needed);
        PSX_CHECK(strstr(text, "PSXTerm diagnostics") != NULL);
        PSX_CHECK(strstr(text, "Group A") != NULL);
        PSX_CHECK(strstr(text, "PASS") != NULL);
        PSX_CHECK(strstr(text, "WARN") != NULL);
        PSX_CHECK(strstr(text, "READY WITH WARNINGS") != NULL);
        PSX_CHECK(strstr(text, "errno") == NULL);
        free(text);
    }

    /* A small buffer must still be NUL-terminated and report the full size */
    {
        char small[16];

        memset(small, 'x', sizeof(small));
        small_len = psx_diag_format_human(&report, small, sizeof(small));
        PSX_CHECK_EQ(small_len, needed);
        PSX_CHECK_EQ(strlen(small), sizeof(small) - 1);
    }

    needed = psx_diag_format_json(&report, NULL, 0);
    PSX_CHECK(needed > 0);

    if((text = malloc(needed + 1))) {
        PSX_CHECK_EQ(psx_diag_format_json(&report, text, needed + 1), needed);
        PSX_CHECK(strstr(text, "\"groups\"") != NULL);
        PSX_CHECK(strstr(text, "\"status\": \"WARN\"") != NULL);
        PSX_CHECK(strstr(text, "\\\"") != NULL);
        PSX_CHECK(strstr(text, "\\\\") != NULL);
        PSX_CHECK(strstr(text, "\\n") != NULL);
        PSX_CHECK(strstr(text, "\"error_code\": 13") != NULL);
        PSX_CHECK(strstr(text, "\"result\": \"READY WITH WARNINGS\"") != NULL);
        free(text);
    }

    psx_diag_report_free(&report);
}

static void
test_spawn_stage_names(void)
{
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_PREPARE), "PREPARE");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_CREATE_VICTIM),
                     "CREATE_VICTIM");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_ATTACH), "ATTACH");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_RAISE_PRIVILEGES),
                     "RAISE_PRIVILEGES");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_DUP_STDIO),
                     "DUP_STDIO");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_LOAD_ELF), "LOAD_ELF");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_RELOCATE), "RELOCATE");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_SET_REGISTERS),
                     "SET_REGISTERS");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_DETACH), "DETACH");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name(PSX_SPAWN_STAGE_RUNNING), "RUNNING");
    PSX_CHECK_STR_EQ(psx_spawn_stage_name((psx_spawn_stage_t)99), "UNKNOWN");
}

static void
test_spawn_failure_reporting(void)
{
    psx_spawn_failure_t failure;
    psx_spawn_options_t options;
    char *const argv[] = {"missing-binary", NULL};

    memset(&failure, 0, sizeof(failure));
    errno = 0;
    PSX_CHECK_EQ(psx_spawn_ex(NULL, &failure), -1);
    PSX_CHECK_EQ(failure.stage, PSX_SPAWN_STAGE_PREPARE);
    PSX_CHECK_EQ(failure.error_code, EINVAL);
    PSX_CHECK(strlen(failure.detail) > 0);

    memset(&options, 0, sizeof(options));
    options.path = "/bin/true";
    PSX_CHECK_EQ(psx_spawn_ex(&options, &failure), -1);
    PSX_CHECK_EQ(failure.error_code, EINVAL);

    memset(&options, 0, sizeof(options));
    options.path = "/nonexistent/psxterm-diag-test";
    options.argv = argv;
    options.stdin_fd = -1;
    options.stdout_fd = -1;
    options.stderr_fd = -1;

    {
        pid_t pid = psx_spawn_ex(&options, &failure);
        int status = 0;

        /* On the host the fork succeeds; the exec failure surfaces through
         * the exit status, and the reported stage stays RUNNING. */
        PSX_CHECK(pid > 0);
        if(pid > 0) {
            PSX_CHECK_EQ(psx_process_wait(pid, &status, 5000), 0);
            PSX_CHECK(WIFEXITED(status));
            PSX_CHECK_EQ(WEXITSTATUS(status), 127);
            PSX_CHECK_EQ(failure.stage, PSX_SPAWN_STAGE_RUNNING);
        }
    }

    {
        char *const true_argv[] = {"true", NULL};
        pid_t pid;
        int status = 0;

        options.path = "/bin/true";
        options.argv = true_argv;

        pid = psx_spawn_ex(&options, &failure);
        PSX_CHECK(pid > 0);
        if(pid > 0) {
            PSX_CHECK_EQ(psx_process_wait(pid, &status, 5000), 0);
            PSX_CHECK(WIFEXITED(status));
            PSX_CHECK_EQ(WEXITSTATUS(status), 0);
        }
    }
}

int
main(void)
{
    test_report_bookkeeping();
    test_overall_status();
    test_status_names();
    test_renderings();
    test_spawn_stage_names();
    test_spawn_failure_reporting();

    return PSX_TEST_SUMMARY();
}
