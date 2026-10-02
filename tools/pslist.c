/*
 * pslist - bring-up helper payload: list processes on the console.
 *
 * Used to decide how a new psxtermd instance can identify the previous one.
 * Uses the same sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PROC) snapshot fields
 * that the reference ELF loaders rely on (ki_structsize, ki_pid, ki_tdname).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/sysctl.h>
#include <sys/types.h>

#include <ps5/klog.h>

int
main(void)
{
    const int mib[4] = {1, 14, 8, 0};
    size_t buf_size = 0;
    uint8_t *buf;

    klog_printf("%s", "pslist: starting\n");
    printf("pslist: pid=%d\n", (int)getpid());

    if(sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0 || buf_size == 0) {
        printf("pslist: sysctl size failed\n");
        return 1;
    }

    if(!(buf = malloc(buf_size))) {
        printf("pslist: out of memory\n");
        return 1;
    }

    if(sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
        printf("pslist: sysctl data failed\n");
        free(buf);
        return 1;
    }

    printf("pslist: %u bytes of process data\n", (unsigned)buf_size);

    for(uint8_t *ptr = buf; ptr < buf + buf_size;) {
        int struct_size = *(int *)ptr;
        int pid = *(int *)&ptr[72];
        char *name = (char *)&ptr[447];

        if(struct_size <= 0) {
            break;
        }

        printf("pid=%5d name=%s\n", pid, name);
        fflush(stdout);

        ptr += struct_size;
    }

    free(buf);
    printf("pslist: done\n");
    klog_printf("%s", "pslist: done\n");

    return 0;
}
