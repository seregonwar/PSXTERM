#include <stdlib.h>
#include <string.h>

#include "psxterm/platform.h"
#include "psxterm/session.h"

#define PSX_ENV_ENTRY_MAX 65536
/* Inherited environments must leave room for cd/export and runtime paths. */
#define PSX_ENV_INHERIT_MAX (PSX_ENV_MAX - 16)

static int
env_index_of(const psx_env_t *env, const char *name, size_t name_len)
{
    for(size_t i = 0; i < env->count; i++) {
        const char *entry = env->entries[i];

        if(strncmp(entry, name, name_len) == 0 && entry[name_len] == '=') {
            return (int)i;
        }
    }

    return -1;
}

void
psx_env_clear(psx_env_t *env)
{
    for(size_t i = 0; i < env->count; i++) {
        free(env->entries[i]);
    }
    memset(env, 0, sizeof(*env));
}

int
psx_env_set(psx_env_t *env, const char *name, const char *value)
{
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    size_t total = name_len + 1 + value_len + 1;
    char *entry;
    int index;

    if(name_len == 0 || total > PSX_ENV_ENTRY_MAX) {
        return -1;
    }

    if(!(entry = malloc(total))) {
        return -1;
    }

    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1);

    if((index = env_index_of(env, name, name_len)) >= 0) {
        free(env->entries[index]);
        env->entries[index] = entry;

        return 0;
    }

    if(env->count >= PSX_ENV_MAX) {
        free(entry);
        return -1;
    }

    env->entries[env->count++] = entry;

    return 0;
}

int
psx_env_set_entry(psx_env_t *env, const char *name_eq_value)
{
    const char *eq = strchr(name_eq_value, '=');
    char name[256];

    if(!eq || eq == name_eq_value) {
        return -1;
    }

    if((size_t)(eq - name_eq_value) >= sizeof(name)) {
        return -1;
    }

    memcpy(name, name_eq_value, (size_t)(eq - name_eq_value));
    name[eq - name_eq_value] = '\0';

    return psx_env_set(env, name, eq + 1);
}

int
psx_env_unset(psx_env_t *env, const char *name)
{
    size_t name_len = strlen(name);
    int index;

    if((index = env_index_of(env, name, name_len)) < 0) {
        return 0;
    }

    free(env->entries[index]);
    for(size_t i = (size_t)index; i + 1 < env->count; i++) {
        env->entries[i] = env->entries[i + 1];
    }
    env->count--;

    return 0;
}

const char *
psx_env_get(const psx_env_t *env, const char *name)
{
    size_t name_len = strlen(name);
    int index = env_index_of(env, name, name_len);

    if(index < 0) {
        return NULL;
    }

    return env->entries[index] + name_len + 1;
}

size_t
psx_env_count(const psx_env_t *env)
{
    return env->count;
}

const char *
psx_env_entry(const psx_env_t *env, size_t index)
{
    if(index >= env->count) {
        return NULL;
    }

    return env->entries[index];
}

void
psx_env_init(psx_env_t *env, char *const *inherit)
{
    memset(env, 0, sizeof(*env));

    psx_env_set(env, "TERM", "xterm-256color");
    psx_env_set(env, "PATH", psx_platform_default_path());
    psx_env_set(env, "HOME", psx_platform_home_dir());
    psx_env_set(env, "USER", psx_platform_user_name());
    psx_env_set(env, "SHELL", "psh");
    psx_env_set(env, "PSXTERM", "1");

    if(inherit) {
        for(char *const *entry = inherit; *entry; entry++) {
            const char *eq = strchr(*entry, '=');

            if(env->count >= PSX_ENV_INHERIT_MAX && eq &&
               env_index_of(env, *entry, (size_t)(eq - *entry)) < 0) {
                continue;
            }
            psx_env_set_entry(env, *entry);
        }
    }

    /* Keep the PSXTerm identity after inheriting the daemon environment. */
    psx_env_set(env, "SHELL", "psh");
    psx_env_set(env, "PSXTERM", "1");
}
