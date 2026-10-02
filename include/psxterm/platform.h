#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Platform-wide primitives. Platform specific implementations live under
 * platform/<host|ps4|ps5>/ and must not leak into core or shell code.
 */

/* One-time platform setup. Returns false on a fatal platform problem. */
bool psx_platform_init(void);

/*
 * Escape the payload sandbox (uid/caps/authid/root vnode) before anything
 * else runs. Derived from the reference privilege manager used by MemDBG:
 * on PS5 the payload is injected into a jailed Sony process, and without this
 * kernel helpers and ptrace are unreliable (spawns hang or fail with EPERM).
 * PS4 deliberately keeps the loader's shared credentials untouched.
 */
bool psx_privilege_raise(void);

/* Human readable platform name, e.g. "PS5", "PS4", "Host". */
const char *psx_platform_name(void);

/*
 * Show a system notification on the console (an on-screen toast, not a log
 * line). No-op with a debug log where the platform has no notification API.
 */
void psx_platform_notify(const char *message);

/* Machine form of the platform name, e.g. "ps5", "ps4", "host". */
const char *psx_platform_id(void);

/* Kernel/platform description used by `uname`. */
const char *psx_platform_uname(void);

/*
 * Console firmware version when it can be discovered reliably, otherwise
 * NULL. Diagnostics report NULL as UNKNOWN; callers must not guess.
 */
const char *psx_platform_firmware(void);

/* Default PATH for new sessions. */
const char *psx_platform_default_path(void);

/* Home directory for new sessions. */
const char *psx_platform_home_dir(void);

/* Default user name reported by `whoami`. */
const char *psx_platform_user_name(void);

/* Directory where PSXTerm keeps its executables, e.g. /data/psxterm/bin. */
const char *psx_platform_bin_dir(void);

/* Cryptographically weak is fine here: session ids and handshake tokens. */
int psx_platform_random_bytes(void *buf, size_t len);

/* Environment inherited by new sessions (NULL-terminated), or NULL. */
char *const *psx_platform_inherit_environ(void);

/*
 * Platform log sink. On consoles the payload's stderr usually leads nowhere,
 * so logs are also forwarded to the kernel log (visible through the klog
 * server); the host implementation does nothing.
 */
void psx_platform_log_line(const char *line);

/*
 * Filesystem preparation, called once during psx_platform_init(). On
 * PS4/PS5 this is where a payload would make /data reachable; the host
 * implementation is a no-op.
 */
bool psx_fs_prepare(void);

/* Default working directory for new sessions. */
const char *psx_fs_default_cwd(void);

bool psx_platform_is_target(void);
