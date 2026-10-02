#include <stdlib.h>
#include <string.h>

#include "psxterm/session.h"

#include "psx_test.h"

static void
test_defaults(void)
{
    psx_env_t env;

    psx_env_init(&env, NULL);

    PSX_CHECK_STR_EQ(psx_env_get(&env, "TERM"), "xterm-256color");
    PSX_CHECK(psx_env_get(&env, "PATH") != NULL);
    PSX_CHECK(psx_env_get(&env, "HOME") != NULL);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "SHELL"), "psh");
    PSX_CHECK_STR_EQ(psx_env_get(&env, "PSXTERM"), "1");
    PSX_CHECK(psx_env_get(&env, "DOES_NOT_EXIST") == NULL);

    psx_env_clear(&env);
    PSX_CHECK_EQ(psx_env_count(&env), 0);
}

static void
test_set_get_unset(void)
{
    psx_env_t env;

    psx_env_init(&env, NULL);

    PSX_CHECK_EQ(psx_env_set(&env, "FOO", "bar"), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "FOO"), "bar");

    /* overwrite */
    PSX_CHECK_EQ(psx_env_set(&env, "FOO", "baz"), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "FOO"), "baz");

    /* set_entry */
    PSX_CHECK_EQ(psx_env_set_entry(&env, "FOO=qux"), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "FOO"), "qux");

    /* entry without '=' is rejected */
    PSX_CHECK_EQ(psx_env_set_entry(&env, "FOO"), -1);
    PSX_CHECK_EQ(psx_env_set_entry(&env, "=value"), -1);

    /* unset */
    PSX_CHECK_EQ(psx_env_unset(&env, "FOO"), 0);
    PSX_CHECK(psx_env_get(&env, "FOO") == NULL);
    PSX_CHECK_EQ(psx_env_unset(&env, "FOO"), 0); /* idempotent */

    psx_env_clear(&env);
}

static void
test_long_values(void)
{
    psx_env_t env;
    char long_value[8192];

    /* Regression: inherited environments (e.g. a WSL PATH) are much longer
     * than 1 KiB and must not be silently dropped. */
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    psx_env_init(&env, NULL);
    PSX_CHECK_EQ(psx_env_set(&env, "LONGPATH", long_value), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "LONGPATH"), long_value);

    psx_env_clear(&env);
}

static void
test_entry_bounds(void)
{
    psx_env_t env;

    psx_env_init(&env, NULL);

    PSX_CHECK(psx_env_entry(&env, psx_env_count(&env)) == NULL);
    PSX_CHECK_EQ(psx_env_set(&env, "", "x"), -1);

    psx_env_clear(&env);
}

int
main(void)
{
    test_defaults();
    test_set_get_unset();
    test_long_values();
    test_entry_bounds();

    return PSX_TEST_SUMMARY();
}
