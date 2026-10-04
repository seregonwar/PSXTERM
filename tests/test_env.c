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

static void
test_large_inherited_environment(void)
{
    psx_env_t env;
    char entries[PSX_ENV_MAX + 10][32];
    char *inherit[PSX_ENV_MAX + 12];

    for(size_t i = 0; i < PSX_ENV_MAX + 10; i++) {
        snprintf(entries[i], sizeof(entries[i]), "RUNNER_%zu=value", i);
        inherit[i] = entries[i];
    }
    /* An existing variable can still be overridden after the import limit. */
    inherit[PSX_ENV_MAX + 10] = "TERM=vt100";
    inherit[PSX_ENV_MAX + 11] = NULL;

    psx_env_init(&env, inherit);
    PSX_CHECK(psx_env_count(&env) <= PSX_ENV_MAX - 16);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "TERM"), "vt100");
    PSX_CHECK_STR_EQ(psx_env_get(&env, "PSXTERM"), "1");
    PSX_CHECK_EQ(psx_env_set(&env, "PWD", "/"), 0);
    PSX_CHECK_EQ(psx_env_set(&env, "OLDPWD", "/tmp"), 0);
    PSX_CHECK_EQ(psx_env_set(&env, "MARK", "alpha"), 0);
    PSX_CHECK_STR_EQ(psx_env_get(&env, "MARK"), "alpha");
    psx_env_clear(&env);
}

int
main(void)
{
    test_defaults();
    test_set_get_unset();
    test_long_values();
    test_entry_bounds();
    test_large_inherited_environment();

    return PSX_TEST_SUMMARY();
}
