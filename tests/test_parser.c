#include <stdlib.h>
#include <string.h>

#include "psxterm/shell.h"

#include "psx_test.h"

static void
check_parse(const char *line, int expected_argc, const char *const *expected)
{
    char **argv = NULL;
    int argc = 0;

    PSX_CHECK_EQ(psh_parse_line(line, &argc, &argv), 0);
    PSX_CHECK_EQ(argc, expected_argc);

    for(int i = 0; i < argc && i < expected_argc; i++) {
        PSX_CHECK_STR_EQ(argv[i], expected[i]);
    }

    PSX_CHECK(argv != NULL);
    if(argv) {
        PSX_CHECK(argv[argc] == NULL);
    }

    psh_argv_free(argc, argv);
}

static void
test_simple(void)
{
    static const char *const expected[] = {"ls", "-l", "/data"};

    check_parse("ls -l /data", 3, expected);
    check_parse("   ls \t -l   /data  ", 3, expected);
}

static void
test_quotes(void)
{
    static const char *const expected[] = {"echo", "argument with spaces"};

    check_parse("echo \"argument with spaces\"", 2, expected);

    {
        static const char *const concat[] = {"echo", "abcdef"};
        check_parse("echo ab\"cd\"ef", 2, concat);
    }

    {
        static const char *const empty[] = {"cmd", ""};
        check_parse("cmd \"\"", 2, empty);
    }
}

static void
test_escapes(void)
{
    {
        static const char *const expected[] = {"echo", "a\"b"};
        check_parse("echo a\\\"b", 2, expected);
    }

    {
        static const char *const expected[] = {"echo", "a\\b"};
        check_parse("echo a\\\\b", 2, expected);
    }

    {
        static const char *const expected[] = {"echo", "a b"};
        check_parse("echo a\\ b", 2, expected);
    }

    {
        static const char *const expected[] = {"echo", "quoted \"x\""};
        check_parse("echo \"quoted \\\"x\\\"\"", 2, expected);
    }
}

static void
test_empty_and_errors(void)
{
    char **argv = NULL;
    int argc = -1;

    PSX_CHECK_EQ(psh_parse_line("", &argc, &argv), 0);
    PSX_CHECK_EQ(argc, 0);
    psh_argv_free(argc, argv);

    PSX_CHECK_EQ(psh_parse_line("   \t  ", &argc, &argv), 0);
    PSX_CHECK_EQ(argc, 0);
    psh_argv_free(argc, argv);

    /* unterminated quote */
    PSX_CHECK_EQ(psh_parse_line("echo \"unterminated", &argc, &argv), -1);
}

static void
test_adjacent_quoted_arguments(void)
{
    static const char *const expected[] = {"a", "b c", "d"};

    check_parse("a \"b c\" d", 3, expected);
}

int
main(void)
{
    test_simple();
    test_quotes();
    test_escapes();
    test_empty_and_errors();
    test_adjacent_quoted_arguments();

    return PSX_TEST_SUMMARY();
}
