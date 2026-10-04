#include <stdio.h>

#include "psxterm/platform.h"
#include "psxterm/tty.h"
#include "psxterm/version.h"

/*
 * Runtime TTY capability probe.
 *
 * Exit status: 0 when the FreeBSDPTY backend is usable, 1 when only the
 * PipeTTY fallback is available, 2 on invalid usage.
 */
int
main(void)
{
    psx_tty_probe_result_t result;
    int available;

    printf("PSXTerm %s TTY probe on %s\n", PSXTERM_VERSION_STRING,
           psx_platform_name());

    psx_tty_probe(&result);
    psx_tty_probe_print(&result, stdout);

    available = psx_tty_probe_usable(&result);

    if(!available) {
        printf("REAL PTY NOT YET AVAILABLE - PipeTTY fallback remains in "
               "use\n");
    }

    return available ? 0 : 1;
}
