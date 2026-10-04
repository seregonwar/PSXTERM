#include <string.h>

#include "psxterm/shell.h"

#include "psx_test.h"

static void
test_expected_builtins(void)
{
    static const char *const names[] = {
        "help", "pwd", "cd",  "ls",   "cat",  "clear", "env",
        "export", "unset", "uname", "whoami", "ps", "exit",
    };

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const psh_command_t *command = psh_registry_lookup(names[i]);

        PSX_CHECK_MSG(command != NULL, "missing builtin: %s", names[i]);
        if(command) {
            PSX_CHECK_STR_EQ(command->name, names[i]);
            PSX_CHECK(command->execute != NULL);
            PSX_CHECK(command->description != NULL);
            PSX_CHECK(strlen(command->description) > 0);
            PSX_CHECK(psh_command_usage(names[i]) != NULL);
        }
    }
}

static void
test_unknown(void)
{
    PSX_CHECK(psh_registry_lookup("not-a-builtin") == NULL);
    PSX_CHECK(psh_registry_lookup("") == NULL);
    PSX_CHECK(psh_registry_lookup("HELP") == NULL); /* case sensitive */
    PSX_CHECK(psh_command_usage("missing") == NULL);
}

static void
test_table_consistency(void)
{
    size_t count = 0;
    const psh_command_t *table = psh_registry_table(&count);

    PSX_CHECK(table != NULL);
    PSX_CHECK(count >= 13);

    for(size_t i = 0; i < count; i++) {
        PSX_CHECK(table[i].name != NULL);
        PSX_CHECK(table[i].execute != NULL);
        /* lookup by name finds the same entry */
        PSX_CHECK(psh_registry_lookup(table[i].name) == &table[i]);
    }
}

int
main(void)
{
    test_expected_builtins();
    test_unknown();
    test_table_consistency();

    return PSX_TEST_SUMMARY();
}
