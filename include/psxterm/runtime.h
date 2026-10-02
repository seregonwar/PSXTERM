#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * PSXTerm runtime layout.
 *
 * Agent CLIs expect a real-looking user environment: a home directory, a
 * temporary directory, XDG locations, a certificate bundle and a PATH that
 * contains the runtime tools. All of that lives under one runtime root, so
 * the paths are defined here once and never hardcoded at call sites.
 *
 * On a console the root is /data/psxterm/runtime; on the host it sits under
 * the same per-user home the rest of PSXTerm uses, which keeps the tests
 * honest about the layout without touching a console.
 */

typedef enum {
    PSX_RUNTIME_DIR_BIN = 0,
    PSX_RUNTIME_DIR_LIB,
    PSX_RUNTIME_DIR_ETC,
    PSX_RUNTIME_DIR_SHARE,
    PSX_RUNTIME_DIR_TMP,
    PSX_RUNTIME_DIR_VAR,
    PSX_RUNTIME_DIR_HOME,
    PSX_RUNTIME_DIR_CACHE,
    PSX_RUNTIME_DIR_AGENTS,
    PSX_RUNTIME_DIR_WORKSPACES,
    PSX_RUNTIME_DIR_COUNT
} psx_runtime_dir_t;

/* Absolute runtime root, without a trailing slash. Never NULL. */
const char *psx_runtime_root(void);

/* Absolute path of one runtime directory, without a trailing slash. */
const char *psx_runtime_dir(psx_runtime_dir_t which);

/* Certificate bundle the runtime ships for TLS clients. */
const char *psx_runtime_ca_bundle(void);

/* Manifest describing what the runtime root contains. */
const char *psx_runtime_manifest_path(void);

/*
 * Create every runtime directory. Idempotent, safe to call on every start.
 * Returns false and sets errno when a directory cannot be created.
 */
bool psx_runtime_prepare(void);

/*
 * Environment every externally spawned CLI process must receive, as
 * "KEY=VALUE" strings: HOME, TMPDIR, the XDG locations, PATH, the TLS bundle
 * variables and TERM. Writes up to max entries plus a NULL terminator into
 * out and returns the number of entries written. The caller owns the
 * strings.
 */
size_t psx_runtime_environment(char **out, size_t max);

typedef struct {
    const char *key;
    const char *value;
} psx_runtime_env_t;

/*
 * The same environment in structured form, for code that has to decide how
 * to merge each key. Pointers stay valid until the next call.
 */
size_t psx_runtime_env_table(psx_runtime_env_t *out, size_t max);

/* Keys the runtime owns: an external CLI must never see a different HOME,
 * TMPDIR, XDG location or certificate bundle than the runtime layout. */
bool psx_runtime_env_is_contract(const char *key);

/*
 * Write the runtime manifest (runtime.json) describing this layout. Metadata
 * only: this is not a package manager.
 */
bool psx_runtime_manifest_write(const char *extra_packages_json);
