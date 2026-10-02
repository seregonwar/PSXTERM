#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process snapshot used by instance replacement. Implemented per platform:
 * Linux/macOS read /proc, PS5 uses the sysctl(KERN_PROC_PROC) snapshot the
 * reference loaders rely on. Platforms that cannot enumerate return 0.
 */
typedef struct {
    int32_t pid;
    char name[64];
} psx_proc_entry_t;

/* Returns the number of entries written, 0 when unsupported, -1 on error. */
int psx_platform_list_processes(psx_proc_entry_t *entries, size_t max_entries);

/*
 * Instance management ("replace the previous payload without rebooting").
 *
 * A PID file in the PSXTerm home records the running daemon so the next
 * launch can terminate it cleanly (SIGTERM, then SIGKILL) before binding the
 * port. The pid file is only honored when the platform can enumerate
 * processes, so a stale PID that was recycled into an unrelated process is
 * never signalled.
 *
 * The very first upgrade of a build that predates the pid file has nothing to
 * read, so a fallback replaces same-name sibling payloads when the port is
 * still busy. Only processes whose name is byte-identical to our own are
 * considered, never our own pid, and never more than a few of them.
 */

/* <PSXTerm home>/psxtermd.pid */
void psx_instance_pid_path(char *out, size_t out_cap);

/* Replaces a recorded previous instance and writes our own pid.
 * Returns 0 when a previous instance was replaced, 1 when there was none,
 * -1 when the pid file could not be written. */
int psx_instance_claim(void);

/* Terminates same-name sibling payloads (bring-up fallback). Returns the
 * number of processes terminated. */
int psx_instance_replace_siblings(void);

/* Our own process name as reported by the platform ("" when unavailable). */
void psx_instance_own_name(char *out, size_t out_cap);

/* Removes the pid file when it still names this process. */
void psx_instance_release(void);
