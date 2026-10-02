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

/* Human readable platform name, e.g. "PS5", "PS4", "Host". */
const char *psx_platform_name(void);

/* Machine form of the platform name, e.g. "ps5", "ps4", "host". */
const char *psx_platform_id(void);

/* Kernel/platform description used by `uname`. */
const char *psx_platform_uname(void);

/* Default PATH for new sessions. */
const char *psx_platform_default_path(void);

/* Directory where PSXTerm keeps its executables, e.g. /data/psxterm/bin. */
const char *psx_platform_bin_dir(void);

/* Cryptographically weak is fine here: session ids and handshake tokens. */
int psx_platform_random_bytes(void *buf, size_t len);

bool psx_platform_is_target(void);
