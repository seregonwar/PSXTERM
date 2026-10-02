/*
 * PS5 process backend.
 *
 * External executables are payload ELFs injected into a freshly created
 * process, following the minimal subset of the ps5-payload-dev/elfldr
 * mechanism that PSXTerm needs (GPLv3, John Törnblom):
 *
 *   rfork_thread(RFPROC | RFCFDG | RFMEM)
 *     child: budget syscall, open /dev/deci_std{in,out,err},
 *            ptrace(PT_TRACE_ME), execve(SceSpZeroConf, argv, envp)
 *   parent: attach, let the eboot reach main(), load the payload ELF into
 *           the stopped process, wire stdio to the session tty, detach.
 *
 * HARDWARE TEST REQUIRED: none of this has been executed on a PS5. The
 * structure mirrors the reference loader, but PSXTerm cannot prove runtime
 * behaviour without hardware.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <machine/reg.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/unistd.h>
#include <sys/wait.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "psxterm/log.h"
#include "psxterm/process.h"
#include "psxterm/util.h"

#define PS5_EBOOT "/system/vsh/app/NPXS40112/eboot.bin"

#ifndef IPV6_2292PKTOPTIONS
#define IPV6_2292PKTOPTIONS 25
#endif

#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x) ((x) & ~(PAGE_SIZE - 1))
#define PFLAGS(x)                                                            \
    ((((x) & PF_R) ? PROT_READ : 0) | (((x) & PF_W) ? PROT_WRITE : 0) |      \
     (((x) & PF_X) ? PROT_EXEC : 0))

/* Single-step loops must not spin forever if the remote never returns, and
 * waits for a stop must not block the whole daemon. */
#define PSX_PT_STEP_LIMIT 4000000u
#define PSX_WAIT_LIMIT_MS 10000

static int
wait_for_stop(pid_t pid, int timeout_ms, int *status_out)
{
    uint64_t deadline = psx_now_ms() + (uint64_t)timeout_ms;

    for(;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if(rc == pid) {
            if(status_out) {
                *status_out = status;
            }
            return 0;
        }
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(psx_now_ms() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};

            nanosleep(&pause, NULL);
        }
    }
}

/* libkernel NIDs used by the loader protocol (same values as elfldr). */
#define NID_SYSCALL "HoLVWNanBBc"
#define NID_PIPE "-Jp7F+pXxNg"
#define NID_GET_PROC_PARAM "959qrazPIrg"

/* PS5-specific syscall used by libkernel to duplicate a foreign fd. */
#define SYS_RDUP 0x25b
#define SYS_PROCESS_NEEDED_AND_RELOCATE 599
#define SYS_BUDGET_SET 0x23b

/* ------------------------------------------------------------------ */
/* remote ptrace helpers                                              */
/* ------------------------------------------------------------------ */

static int
sys_ptrace(int request, pid_t pid, caddr_t addr, int data)
{
    pid_t mypid = getpid();
    uint64_t authid;
    int ret;

    if(!(authid = kernel_get_ucred_authid(mypid))) {
        return -1;
    }

    if(kernel_set_ucred_authid(mypid, 0x4800000000010003l)) {
        return -1;
    }

    ret = (int)syscall(SYS_ptrace, request, pid, addr, data);

    if(kernel_set_ucred_authid(mypid, authid)) {
        return -1;
    }

    return ret;
}

static int
pt_attach(pid_t pid)
{
    if(sys_ptrace(PT_ATTACH, pid, 0, 0) == -1) {
        return -1;
    }

    if(wait_for_stop(pid, PSX_WAIT_LIMIT_MS, NULL) != 0) {
        return -1;
    }

    return 0;
}

static int
pt_detach(pid_t pid, int sig)
{
    return sys_ptrace(PT_DETACH, pid, 0, sig);
}

static int
pt_step(pid_t pid)
{
    if(sys_ptrace(PT_STEP, pid, (caddr_t)1, 0)) {
        return -1;
    }

    if(waitpid(pid, 0, 0) < 0) {
        return -1;
    }

    return 0;
}

static int
pt_continue(pid_t pid, int sig)
{
    return sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig);
}

static int
pt_getregs(pid_t pid, struct reg *r)
{
    return sys_ptrace(PT_GETREGS, pid, (caddr_t)r, 0);
}

static int
pt_setregs(pid_t pid, const struct reg *r)
{
    return sys_ptrace(PT_SETREGS, pid, (caddr_t)r, 0);
}

static int
pt_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len)
{
    struct ptrace_io_desc iod = {
        .piod_op = PIOD_WRITE_D,
        .piod_offs = (void *)addr,
        .piod_addr = (void *)buf,
        .piod_len = len,
    };

    return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static int
pt_copyout(pid_t pid, intptr_t addr, void *buf, size_t len)
{
    struct ptrace_io_desc iod = {
        .piod_op = PIOD_READ_D,
        .piod_offs = (void *)addr,
        .piod_addr = buf,
        .piod_len = len,
    };

    return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static int
pt_setlong(pid_t pid, intptr_t addr, long val)
{
    return pt_copyin(pid, &val, addr, sizeof(val));
}

static int
pt_setint(pid_t pid, intptr_t addr, int val)
{
    return pt_copyin(pid, &val, addr, sizeof(val));
}

static intptr_t
pt_resolve(pid_t pid, const char *nid)
{
    intptr_t addr;

    if((addr = kernel_dynlib_resolve(pid, 0x1, nid))) {
        return addr;
    }

    return kernel_dynlib_resolve(pid, 0x2001, nid);
}

static long
pt_call(pid_t pid, intptr_t addr, ...)
{
    struct reg jmp_reg;
    struct reg bak_reg;
    va_list ap;

    if(pt_getregs(pid, &bak_reg)) {
        return -1;
    }

    memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
    jmp_reg.r_rip = addr;

    va_start(ap, addr);
    jmp_reg.r_rdi = va_arg(ap, uint64_t);
    jmp_reg.r_rsi = va_arg(ap, uint64_t);
    jmp_reg.r_rdx = va_arg(ap, uint64_t);
    jmp_reg.r_rcx = va_arg(ap, uint64_t);
    jmp_reg.r_r8 = va_arg(ap, uint64_t);
    jmp_reg.r_r9 = va_arg(ap, uint64_t);
    va_end(ap);

    if(pt_setregs(pid, &jmp_reg)) {
        return -1;
    }

    {
        unsigned steps = 0;

        while(jmp_reg.r_rsp <= bak_reg.r_rsp) {
            if(++steps > PSX_PT_STEP_LIMIT) {
                errno = ETIMEDOUT;
                return -1;
            }
            if(pt_step(pid)) {
                return -1;
            }
            if(pt_getregs(pid, &jmp_reg)) {
                return -1;
            }
        }
    }

    if(pt_setregs(pid, &bak_reg)) {
        return -1;
    }

    return jmp_reg.r_rax;
}

static long
pt_syscall(pid_t pid, int sysno, ...)
{
    intptr_t addr = pt_resolve(pid, NID_SYSCALL);
    struct reg jmp_reg;
    struct reg bak_reg;
    va_list ap;

    if(!addr) {
        errno = ENOENT;
        return -1;
    }

    addr += 0xa;

    if(pt_getregs(pid, &bak_reg)) {
        return -1;
    }

    memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
    jmp_reg.r_rip = addr;
    jmp_reg.r_rax = sysno;

    va_start(ap, sysno);
    jmp_reg.r_rdi = va_arg(ap, uint64_t);
    jmp_reg.r_rsi = va_arg(ap, uint64_t);
    jmp_reg.r_rdx = va_arg(ap, uint64_t);
    jmp_reg.r_r10 = va_arg(ap, uint64_t);
    jmp_reg.r_r8 = va_arg(ap, uint64_t);
    jmp_reg.r_r9 = va_arg(ap, uint64_t);
    va_end(ap);

    if(pt_setregs(pid, &jmp_reg)) {
        return -1;
    }

    {
        unsigned steps = 0;

        while(jmp_reg.r_rsp <= bak_reg.r_rsp) {
            if(++steps > PSX_PT_STEP_LIMIT) {
                errno = ETIMEDOUT;
                return -1;
            }
            if(pt_step(pid)) {
                return -1;
            }
            if(pt_getregs(pid, &jmp_reg)) {
                return -1;
            }
        }
    }

    if(pt_setregs(pid, &bak_reg)) {
        return -1;
    }

    return jmp_reg.r_rax;
}

static intptr_t
pt_mmap(pid_t pid, intptr_t addr, size_t len, int prot, int flags, int fd,
        off_t off)
{
    return pt_syscall(pid, SYS_mmap, addr, len, prot, flags, fd, off);
}

static int
pt_mprotect(pid_t pid, intptr_t addr, size_t len, int prot)
{
    return (int)pt_syscall(pid, SYS_mprotect, addr, len, prot);
}

static int
pt_munmap(pid_t pid, intptr_t addr, size_t len)
{
    return (int)pt_syscall(pid, SYS_munmap, addr, len);
}

static int
pt_msync(pid_t pid, intptr_t addr, size_t len, int flags)
{
    return (int)pt_syscall(pid, SYS_msync, addr, len, flags);
}

static int
pt_close(pid_t pid, int fd)
{
    return (int)pt_syscall(pid, SYS_close, fd);
}

static int
pt_dup2(pid_t pid, int oldfd, int newfd)
{
    return (int)pt_syscall(pid, SYS_dup2, oldfd, newfd);
}

static int
pt_rdup(pid_t pid, pid_t other_pid, int fd)
{
    return (int)pt_syscall(pid, SYS_RDUP, other_pid, fd);
}

static int
pt_socket(pid_t pid, int domain, int type, int protocol)
{
    return (int)pt_syscall(pid, SYS_socket, domain, type, protocol);
}

static int
pt_setsockopt(pid_t pid, int fd, int level, int optname, intptr_t optval,
              socklen_t optlen)
{
    return (int)pt_syscall(pid, SYS_setsockopt, fd, level, optname, optval,
                           optlen, 0);
}

/* ------------------------------------------------------------------ */
/* child entry used by rfork_thread                                   */
/* ------------------------------------------------------------------ */

struct ps5_spawn_context {
    char *const *argv;
    char *const *envp;
    const char *path;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
};

/*
 * Copy an argument vector, strings included, into a shared heap arena.
 *
 * The child created by rfork reads this vector, and reading strings that live
 * on the caller's stack produced garbage (hardware-verified: the child logged
 * argc=16 with an unreadable argv[0] while a string literal in .rodata came
 * through fine). Everything the child needs therefore lives in the arena.
 */
static char **
child_copy_vector(char *const *src, size_t max, char **arena, size_t *left)
{
    size_t count = 0;
    size_t need = 0;
    char **out;
    char *strings;

    if(!src) {
        return NULL;
    }

    while(count < max && src[count]) {
        need += strlen(src[count]) + 1;
        count++;
    }

    if((count + 1) * sizeof(char *) + need > *left) {
        return NULL;
    }

    out = (char **)(void *)*arena;
    strings = (char *)(out + count + 1);

    for(size_t i = 0; i < count; i++) {
        size_t len = strlen(src[i]) + 1;

        memcpy(strings, src[i], len);
        out[i] = strings;
        strings += len;
    }
    out[count] = NULL;

    *arena += (count + 1) * sizeof(char *) + need;
    *left -= (count + 1) * sizeof(char *) + need;

    return out;
}

/*
 * Child-side breadcrumb: klog is unreliable here, so the same line also goes
 * to a file in the writable data directory, where it can simply be pulled.
 */
static void
child_note(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int len;
    int fd;

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if(len < 0) {
        return;
    }
    if((size_t)len >= sizeof(line)) {
        len = (int)sizeof(line) - 1;
    }

    klog_printf("%s\n", line);

    fd = open("/data/psxterm/child.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(fd >= 0) {
        ssize_t ignored = write(fd, line, (size_t)len);
        (void)ignored;
        ignored = write(fd, "\n", 1);
        (void)ignored;
        close(fd);
    }
}

static int
ps5_rfork_entry(void *arg)
{
    struct ps5_spawn_context *ctx = arg;

    if(syscall(SYS_BUDGET_SET, 0) != 0) {
        klog_perror("psxterm: budget");
        return -1;
    }

    /* Give the hijacked process a console for the payload's early output. */
    if(open("/dev/deci_stdin", O_RDONLY) < 0 ||
       open("/dev/deci_stdout", O_WRONLY) < 0 ||
       open("/dev/deci_stderr", O_WRONLY) < 0) {
        klog_perror("psxterm: deci stdio");
        return -1;
    }

    if(ptrace(PT_TRACE_ME, 0, 0, 0)) {
        klog_perror("psxterm: ptrace");
        return -1;
    }

    /*
     * Replace the standard descriptors with the session ones here, in the
     * child, while the inherited descriptor table is still valid: nothing
     * beyond 0/1/2 survives execve on this kernel, so this is where the
     * payload's stdio has to be installed. Set PSXTERM_PS5_CHILD_STDIO=0 to
     * fall back to the parent-side dup2/rdup path.
     */
    {
        const char *opt = getenv("PSXTERM_PS5_CHILD_STDIO");

        if(opt == NULL || opt[0] != '0') {
            if(ctx->stdin_fd >= 0) {
                dup2(ctx->stdin_fd, STDIN_FILENO);
            }
            if(ctx->stdout_fd >= 0) {
                dup2(ctx->stdout_fd, STDOUT_FILENO);
            }
            if(ctx->stderr_fd >= 0) {
                dup2(ctx->stderr_fd, STDERR_FILENO);
            }
        }
    }

    /*
     * Report exactly what the exec will hand the console process: the
     * payload inherits this argument vector, and a nonsense count here shows
     * up as a nonsense argc inside the payload.
     */
    {
        int argc = 0;
        const char *first = "(none)";

        while(argc < 16 && ctx->argv && ctx->argv[argc]) {
            if(argc == 0) {
                first = ctx->argv[0];
            }
            argc++;
        }

        klog_printf("psxterm: child spawn: argc=%d argv0=%s path=%s\n", argc,
                    first, ctx->path);
        child_note("child spawn: ctx=%lx argv=%lx argv0=%lx argc=%d first=%s",
                   (unsigned long)(uintptr_t)ctx,
                   (unsigned long)(uintptr_t)ctx->argv,
                   (unsigned long)(uintptr_t)(ctx->argv ? ctx->argv[0] : NULL),
                   argc, first);
    }

    execve(ctx->path, ctx->argv, ctx->envp);
    klog_perror("psxterm: execve");

    return -1;
}

/* ------------------------------------------------------------------ */
/* payload loading                                                    */
/* ------------------------------------------------------------------ */

static int
elf_sanity_check(const uint8_t *elf, size_t elf_size)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    const Elf64_Phdr *phdr;

    if(elf_size < sizeof(Elf64_Ehdr) ||
       elf_size < sizeof(Elf64_Phdr) + ehdr->e_phoff ||
       elf_size < sizeof(Elf64_Shdr) + ehdr->e_shoff) {
        return -1;
    }

    if(ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
       ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        return -1;
    }

    phdr = (const Elf64_Phdr *)(elf + ehdr->e_phoff);
    for(int i = 0; i < ehdr->e_phnum; i++) {
        if(phdr[i].p_offset + phdr[i].p_filesz > elf_size) {
            return -1;
        }
    }

    return 0;
}

static uint8_t *
read_file(const char *path, size_t *size_out)
{
    uint8_t *data = NULL;
    size_t size = 0;
    size_t cap = 0;
    int fd;

    if((fd = open(path, O_RDONLY)) < 0) {
        return NULL;
    }

    for(;;) {
        ssize_t n;

        if(size == cap) {
            uint8_t *grown;
            size_t new_cap = cap ? cap * 2 : 65536;

            if(!(grown = realloc(data, new_cap))) {
                free(data);
                close(fd);
                return NULL;
            }
            data = grown;
            cap = new_cap;
        }

        n = read(fd, data + size, cap - size);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            free(data);
            close(fd);
            return NULL;
        }
        if(n == 0) {
            break;
        }
        size += (size_t)n;
    }

    close(fd);
    *size_out = size;

    return data;
}

static intptr_t
load_elf(pid_t pid, const uint8_t *elf, size_t elf_size)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)(elf + ehdr->e_phoff);
    const Elf64_Shdr *shdr = (const Elf64_Shdr *)(elf + ehdr->e_shoff);
    size_t min_vaddr = (size_t)-1;
    size_t max_vaddr = 0;
    uint8_t *mirror;
    intptr_t base_addr;
    size_t base_size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int error = 0;

    if(elf_sanity_check(elf, elf_size) != 0) {
        errno = ENOEXEC;
        return 0;
    }

    for(int i = 0; i < ehdr->e_phnum; i++) {
        if(phdr[i].p_vaddr < min_vaddr) {
            min_vaddr = phdr[i].p_vaddr;
        }
        if(max_vaddr < phdr[i].p_vaddr + phdr[i].p_memsz) {
            max_vaddr = phdr[i].p_vaddr + phdr[i].p_memsz;
        }
    }

    min_vaddr = TRUNC_PG(min_vaddr);
    max_vaddr = ROUND_PG(max_vaddr);
    base_size = max_vaddr - min_vaddr;

    if(ehdr->e_type == ET_DYN) {
        base_addr = 0;
    } else if(ehdr->e_type == ET_EXEC) {
        base_addr = (intptr_t)min_vaddr;
        flags |= MAP_FIXED;
    } else {
        errno = ENOEXEC;
        return 0;
    }

    if(!(mirror = malloc(base_size))) {
        return 0;
    }
    memset(mirror, 0, base_size);

    if((base_addr = pt_mmap(pid, base_addr, base_size,
                            PROT_READ | PROT_WRITE, flags, -1, 0)) == -1) {
        free(mirror);
        return 0;
    }

    for(int i = 0; i < ehdr->e_phnum && !error; i++) {
        if(phdr[i].p_type == PT_LOAD && phdr[i].p_filesz) {
            memcpy(mirror + phdr[i].p_vaddr, elf + phdr[i].p_offset,
                   phdr[i].p_filesz);
        }
    }

    for(int i = 0; i < ehdr->e_shnum && !error; i++) {
        const Elf64_Rela *rela;
        size_t count;

        if(shdr[i].sh_type != SHT_RELA) {
            continue;
        }

        rela = (const Elf64_Rela *)(elf + shdr[i].sh_offset);
        count = shdr[i].sh_size / sizeof(Elf64_Rela);

        for(size_t j = 0; j < count; j++) {
            if((rela[j].r_info & 0xffffffffl) == R_X86_64_RELATIVE) {
                intptr_t *loc = (intptr_t *)(mirror + rela[j].r_offset);
                *loc = base_addr + rela[j].r_addend;
            }
        }
    }

    if(pt_copyin(pid, mirror, base_addr, base_size)) {
        error = 1;
    }

    for(int i = 0; i < ehdr->e_phnum && !error; i++) {
        intptr_t addr;

        if(phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }

        addr = base_addr + phdr[i].p_vaddr;

        if(phdr[i].p_flags & PF_X) {
            if(kernel_mprotect(pid, addr, ROUND_PG(phdr[i].p_memsz),
                               PFLAGS(phdr[i].p_flags))) {
                error = 1;
            }
        } else if(pt_mprotect(pid, addr, ROUND_PG(phdr[i].p_memsz),
                              PFLAGS(phdr[i].p_flags))) {
            error = 1;
        }
    }

    if(pt_msync(pid, base_addr, base_size, MS_SYNC)) {
        error = 1;
    }

    free(mirror);

    if(error) {
        pt_munmap(pid, base_addr, base_size);
        return 0;
    }

    return base_addr + ehdr->e_entry;
}

/*
 * Build the payload argument block the SDK crt expects in rdi. This mirrors
 * the argument construction used by the reference ELF loader.
 */
static intptr_t
build_payload_args(pid_t pid)
{
    intptr_t buf;
    int master_sock;
    int victim_sock;
    int pipe_fds[2] = {-1, -1};
    intptr_t kpipe_addr;

    if((buf = pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
        PSX_LOGE("ps5: args: pt_mmap failed");
        return 0;
    }

    PSX_LOGI("ps5: args: page 0x%lx", (unsigned long)buf);

    if((master_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        PSX_LOGE("ps5: args: master socket failed");
        return 0;
    }

    PSX_LOGI("ps5: args: master socket %d", master_sock);

    pt_setint(pid, buf + 0x00, 20);
    pt_setint(pid, buf + 0x04, IPPROTO_IPV6);
    pt_setint(pid, buf + 0x08, IPV6_TCLASS);
    pt_setint(pid, buf + 0x0c, 0);
    pt_setint(pid, buf + 0x10, 0);
    pt_setint(pid, buf + 0x14, 0);

    if(pt_setsockopt(pid, master_sock, IPPROTO_IPV6, IPV6_2292PKTOPTIONS, buf,
                     24)) {
        PSX_LOGE("ps5: args: master setsockopt failed");
        return 0;
    }

    PSX_LOGI("ps5: args: master options set");

    if((victim_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        PSX_LOGE("ps5: args: victim socket failed");
        return 0;
    }

    PSX_LOGI("ps5: args: victim socket %d", victim_sock);

    pt_setint(pid, buf + 0x00, 0);
    pt_setint(pid, buf + 0x04, 0);
    pt_setint(pid, buf + 0x08, 0);
    pt_setint(pid, buf + 0x0c, 0);
    pt_setint(pid, buf + 0x10, 0);

    if(pt_setsockopt(pid, victim_sock, IPPROTO_IPV6, IPV6_PKTINFO, buf, 20)) {
        PSX_LOGE("ps5: args: victim setsockopt failed");
        return 0;
    }

    PSX_LOGI("ps5: args: victim options set");

    if(kernel_overlap_sockets(pid, master_sock, victim_sock)) {
        PSX_LOGE("ps5: args: kernel_overlap_sockets failed");
        return 0;
    }

    PSX_LOGI("ps5: args: sockets overlapped, creating pipe");

    /*
     * SAFETY RULE, learned the hard way (two kernel panics on hardware):
     * rwpipe and kpipe_addr MUST describe a real pipe created inside the
     * victim. Handing the payload runtime any other descriptor - a session
     * socket, for instance - makes it treat that kernel file object as a pipe
     * and corrupt kernel memory.
     *
     * The runtime reads its input from rwpipe[0] while starting, so the pipe
     * must exist and stay empty until the loader relays something into it.
     * Relaying through the daemon's own end of this pipe is the next step;
     * until then the private pipe is the only safe configuration.
     */
    {
        intptr_t victim_fds = buf + 0x400;

        if(pt_call(pid, pt_resolve(pid, NID_PIPE), (uint64_t)victim_fds) != 0) {
            PSX_LOGE("ps5: args: pipe call failed");
            return 0;
        }

        pipe_fds[0] = -1;
        pipe_fds[1] = -1;

        if(pt_copyout(pid, victim_fds, pipe_fds, sizeof(pipe_fds))) {
            PSX_LOGE("ps5: args: cannot read the created pipe fds");
            return 0;
        }
    }

    if(pipe_fds[0] < 0 || pipe_fds[1] < 0) {
        PSX_LOGE("ps5: args: refusing a non-pipe runtime handle");
        return 0;
    }

    PSX_LOGI("ps5: args: pipe %d,%d", pipe_fds[0], pipe_fds[1]);

    kpipe_addr = kernel_get_proc_file(pid, pipe_fds[0]);
    if(!kpipe_addr) {
        PSX_LOGE("ps5: args: kernel_get_proc_file failed");
        return 0;
    }

    {
        intptr_t args = buf;
        intptr_t rwpipe = buf + 0x100;
        intptr_t rwpair = buf + 0x200;
        intptr_t payloadout = buf + 0x300;

        pt_setlong(pid, args + 0x00, pt_resolve(pid, NID_SYSCALL));
        pt_setlong(pid, args + 0x08, rwpipe);
        pt_setlong(pid, args + 0x10, rwpair);
        pt_setlong(pid, args + 0x18, kpipe_addr);
        pt_setlong(pid, args + 0x20, KERNEL_ADDRESS_DATA_BASE);
        pt_setlong(pid, args + 0x28, payloadout);

        pt_setint(pid, rwpipe + 0, pipe_fds[0]);
        pt_setint(pid, rwpipe + 4, pipe_fds[1]);
        pt_setint(pid, rwpair + 0, master_sock);
        pt_setint(pid, rwpair + 4, victim_sock);
        pt_setint(pid, payloadout, 0);

        return args;
    }
}

/* Give the payload's libc an unlimited heap (reference-loader behaviour). */
static void
set_heap_size(pid_t pid, int size)
{
    intptr_t proc_param;
    intptr_t libc_param;
    intptr_t heap_size_addr;

    if(!(proc_param = pt_call(pid, pt_resolve(pid, NID_GET_PROC_PARAM)))) {
        return;
    }

    if(pt_copyout(pid, proc_param + 56, &libc_param, sizeof(libc_param))) {
        return;
    }

    if(pt_copyout(pid, libc_param + 16, &heap_size_addr,
                  sizeof(heap_size_addr))) {
        return;
    }

    pt_setint(pid, heap_size_addr, size);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

static void
stage_set(psx_spawn_failure_t *failure, psx_spawn_stage_t stage, int error_code,
          const char *detail)
{
    if(!failure) {
        return;
    }

    failure->stage = stage;
    failure->error_code = error_code;
    failure->detail[0] = '\0';

    if(detail) {
        snprintf(failure->detail, sizeof(failure->detail), "%s", detail);
    }
}

/*
 * The victim is a jailed Sony process, and the reference loader lifts its
 * sandbox before touching its descriptors. Timing matters on this firmware:
 * done after the ptrace exec stop the victim hangs, so the caller does it
 * while the victim is still running and untraced.
 */
static void
raise_target_privileges(pid_t pid)
{
    const uint64_t system_authid = 0x4801000000000013ull;
    uint8_t caps[16];
    intptr_t root_vnode;

    memset(caps, 0xff, sizeof(caps));

    root_vnode = kernel_get_root_vnode();
    if(root_vnode != 0) {
        kernel_set_proc_rootdir(pid, root_vnode);
        kernel_set_proc_jaildir(pid, 0);
    }

    /* Same fields the reference loader writes on its victim, authid
     * included: without it the target stays in the Sony credential class. */
    kernel_set_ucred_uid(pid, 0);
    kernel_set_ucred_ruid(pid, 0);
    kernel_set_ucred_svuid(pid, 0);
    kernel_set_ucred_rgid(pid, 0);
    kernel_set_ucred_svgid(pid, 0);
    kernel_set_ucred_authid(pid, system_authid);
    kernel_set_ucred_caps(pid, caps);
}

/*
 * The reference loader lifts exactly these fields on its victim immediately
 * before duplicating stdio, while it is stopped at the eboot entry
 * breakpoint. Kept separate from the broader self-escalation set above
 * because the extra credential fields may be what stalled the victim when
 * they were written at this point.
 */
static void
raise_target_privileges_minimal(pid_t pid)
{
    uint8_t caps[16];
    intptr_t root_vnode;

    memset(caps, 0xff, sizeof(caps));

    root_vnode = kernel_get_root_vnode();
    if(root_vnode == 0) {
        return;
    }

    kernel_set_proc_rootdir(pid, root_vnode);
    kernel_set_proc_jaildir(pid, 0);
    kernel_set_ucred_uid(pid, 0);
    kernel_set_ucred_caps(pid, caps);
}

/*
 * Duplicate the daemon-owned stdio descriptors into the victim process.
 *
 * RFCFDG hands the child a copy of our descriptor table and execve keeps
 * every non-CLOEXEC descriptor, so the session fds are normally already
 * present in the victim under the same numbers: clear CLOEXEC before the
 * fork and dup2 directly. The rdup syscall is kept as the fallback for the
 * case where the numbers did not survive.
 */
static int
dup_stdio(pid_t pid, pid_t owner, const psx_spawn_options_t *options,
          psx_spawn_failure_t *failure)
{
    int in_fd = options->stdin_fd;
    int out_fd = options->stdout_fd;
    int err_fd = options->stderr_fd;
    char detail[128];

    {
        int rc = (int)pt_dup2(pid, in_fd, STDIN_FILENO);

        if(rc < 0) {
            int fallback = (int)pt_rdup(pid, owner, options->stdin_fd);

            if(fallback < 0) {
                snprintf(detail, sizeof(detail),
                         "stdin duplication failed (dup2=%d rdup=%d)", rc,
                         fallback);
                stage_set(failure, PSX_SPAWN_STAGE_DUP_STDIO, errno, detail);
                return -1;
            }

            in_fd = fallback;
            pt_dup2(pid, in_fd, STDIN_FILENO);
        }
    }

    if(pt_dup2(pid, out_fd, STDOUT_FILENO) < 0) {
        out_fd = pt_rdup(pid, owner, options->stdout_fd);
        if(out_fd < 0) {
            stage_set(failure, PSX_SPAWN_STAGE_DUP_STDIO, errno,
                      "stdout duplication failed");
            return -1;
        }
        pt_dup2(pid, out_fd, STDOUT_FILENO);
    }

    if(pt_dup2(pid, err_fd, STDERR_FILENO) < 0) {
        err_fd = pt_rdup(pid, owner, options->stderr_fd);
        if(err_fd < 0) {
            stage_set(failure, PSX_SPAWN_STAGE_DUP_STDIO, errno,
                      "stderr duplication failed");
            return -1;
        }
        pt_dup2(pid, err_fd, STDERR_FILENO);
    }

    if(out_fd != in_fd) {
        pt_close(pid, out_fd);
    }
    if(err_fd != in_fd && err_fd != out_fd) {
        pt_close(pid, err_fd);
    }
    pt_close(pid, in_fd);

    return 0;
}

pid_t
psx_platform_spawn(const psx_spawn_options_t *options,
                   psx_spawn_failure_t *failure)
{
    static const uint8_t int3 = 0xcc;
    struct ps5_spawn_context ctx;
    struct ps5_spawn_context *child_ctx;
    struct reg regs;
    uint8_t *elf;
    size_t elf_size = 0;
    intptr_t entry;
    intptr_t args;
    intptr_t brkpoint;
    uint8_t original_byte;
    uint8_t *stack;
    pid_t pid;
    pid_t mypid = getpid();

    stage_set(failure, PSX_SPAWN_STAGE_PREPARE, 0, NULL);

    if(!options->stdin_fd && !options->stdout_fd) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, EINVAL,
                  "no stdio descriptors");
        errno = EINVAL;
        return -1;
    }

    if(!(elf = read_file(options->path, &elf_size))) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, errno,
                  "cannot read the executable");
        return -1;
    }

    if(elf_sanity_check(elf, elf_size) != 0) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, ENOEXEC,
                  "not a valid ELF64 payload");
        free(elf);
        errno = ENOEXEC;
        return -1;
    }

    if(!(stack = malloc(PAGE_SIZE))) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, errno, "out of memory");
        free(elf);
        return -1;
    }

    if(!(child_ctx = malloc(sizeof(*child_ctx)))) {
        stage_set(failure, PSX_SPAWN_STAGE_PREPARE, errno, "out of memory");
        free(stack);
        free(elf);
        return -1;
    }

    ctx.argv = options->argv;
    ctx.envp = options->envp;
    ctx.path = PS5_EBOOT;

    /* Hand the child its own copy of both vectors, in memory it can read. */
    {
        size_t arena_size = 16 * 1024;
        char *arena = calloc(1, arena_size);

        if(arena) {
            char **argv_copy;
            char **envp_copy;
            size_t left = arena_size;

            argv_copy = child_copy_vector(options->argv, PSX_SPAWN_MAX_ARGS,
                                          &arena, &left);
            envp_copy = child_copy_vector(options->envp, PSX_SPAWN_MAX_ENV,
                                          &arena, &left);

            if(argv_copy) {
                ctx.argv = argv_copy;
            }
            if(envp_copy) {
                ctx.envp = envp_copy;
            }
        }
    }
    ctx.stdin_fd = options->stdin_fd;
    ctx.stdout_fd = options->stdout_fd;
    ctx.stderr_fd = options->stderr_fd;
    *child_ctx = ctx;

    /*
     * The victim keeps our session descriptors across execve (RFCFDG copies
     * the table; execve only drops CLOEXEC ones), which is what makes the
     * direct dup2 in dup_stdio() work.
     */
    psx_set_cloexec(options->stdin_fd, false);
    if(options->stdout_fd != options->stdin_fd) {
        psx_set_cloexec(options->stdout_fd, false);
    }
    if(options->stderr_fd != options->stdin_fd &&
       options->stderr_fd != options->stdout_fd) {
        psx_set_cloexec(options->stderr_fd, false);
    }

    stage_set(failure, PSX_SPAWN_STAGE_CREATE_VICTIM, 0, NULL);

    if((pid = rfork_thread(RFPROC | RFCFDG | RFMEM, stack + PAGE_SIZE - 8,
                           ps5_rfork_entry, child_ctx)) < 0) {
        stage_set(failure, PSX_SPAWN_STAGE_CREATE_VICTIM, errno,
                  "rfork_thread failed");
        free(child_ctx);
        free(stack);
        free(elf);
        return -1;
    }

    free(child_ctx);
    free(stack);

    stage_set(failure, PSX_SPAWN_STAGE_ATTACH, 0, NULL);

    /*
     * Lift the victim's own sandbox here, while it is still running and
     * untraced: doing the same after the ptrace exec stop hangs the victim on
     * this firmware (hardware-verified), while the reference privilege
     * manager elevates targets before attaching to them.
     */
    raise_target_privileges(pid);

    /* Wait for the child to reach the exec stop. */
    {
        int status = 0;

        if(wait_for_stop(pid, PSX_WAIT_LIMIT_MS, &status) != 0) {
            stage_set(failure, PSX_SPAWN_STAGE_ATTACH, errno,
                      "victim never stopped (ptrace/execve blocked?)");
            PSX_LOGE("ps5: victim did not stop: %s", strerror(errno));
            kill(pid, SIGKILL);
            free(elf);
            return -1;
        }

        if(!WIFSTOPPED(status)) {
            char detail[128];

            snprintf(detail, sizeof(detail),
                     "victim exited before attach (status %d, exit %d)",
                     status,
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            stage_set(failure, PSX_SPAWN_STAGE_ATTACH, 0, detail);
            PSX_LOGE("ps5: %s", detail);
            free(elf);
            return -1;
        }
    }

    /*
     * No PT_ATTACH here: the child declares PT_TRACE_ME before execve, so we
     * are already its tracer and the stop we just reaped is the attach point.
     * The reference loader does the same (it only attaches on PS4, where the
     * mechanism differs); calling PT_ATTACH on an already-traced process is
     * what made the first hardware spawns fail at ATTACH.
     */

    /* Let the kernel assign process parameters to the eboot. */
    if(pt_syscall(pid, SYS_PROCESS_NEEDED_AND_RELOCATE)) {
        PSX_LOGW("ps5: process_needed_and_relocate failed");
    }

    set_heap_size(pid, -1);

    stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, 0, NULL);

    /* Step past the eboot's own startup (libc initialisation). */
    if(!(brkpoint = kernel_dynlib_entry_addr(pid, 0))) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, ENOENT,
                  "cannot locate the eboot entry");
        PSX_LOGE("ps5: cannot locate the eboot entry");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }
    brkpoint += 58;

    if(kernel_mprotect(pid, brkpoint, PAGE_SIZE,
                       PROT_READ | PROT_WRITE | PROT_EXEC)) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "cannot make the eboot entry writable");
        PSX_LOGE("ps5: mprotect(entry) failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_copyout(pid, brkpoint, &original_byte, sizeof(original_byte)) ||
       pt_copyin(pid, &int3, brkpoint, sizeof(int3))) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "cannot set the entry breakpoint");
        PSX_LOGE("ps5: cannot set the entry breakpoint");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_continue(pid, SIGCONT)) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "continue to the eboot entry failed");
        PSX_LOGE("ps5: continue to entry failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(wait_for_stop(pid, PSX_WAIT_LIMIT_MS, NULL) != 0) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "eboot entry breakpoint never hit");
        PSX_LOGE("ps5: entry breakpoint timeout");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_copyin(pid, &original_byte, brkpoint, sizeof(original_byte))) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "cannot restore the eboot entry byte");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    /* Wire stdio to the session tty before starting the payload. */
    stage_set(failure, PSX_SPAWN_STAGE_DUP_STDIO, 0, NULL);

    /*
     * The child already replaced its standard descriptors with the session
     * ones before execve (see ps5_rfork_entry): if they survived, this only
     * re-points 0/1/2 at the same objects, and a failure here is expected on
     * kernels that close everything past the standard descriptors. Either way
     * the payload's stdio is already correct, so it is not fatal.
     */
    raise_target_privileges_minimal(pid);

    if(dup_stdio(pid, mypid, options, failure) < 0) {
        PSX_LOGI("ps5: parent-side stdio install skipped: %s", failure->detail);
    }

    stage_set(failure, PSX_SPAWN_STAGE_LOAD_ELF, 0, NULL);

    PSX_LOGI("ps5: load_elf: start");

    if(!(entry = load_elf(pid, elf, elf_size))) {
        stage_set(failure, PSX_SPAWN_STAGE_LOAD_ELF, errno,
                  "payload mapping, copy or relocation failed");
        PSX_LOGE("ps5: ELF load failed: %s", strerror(errno));
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    PSX_LOGI("ps5: load_elf: done entry=0x%lx", (unsigned long)entry);
    PSX_LOGI("ps5: payload args: start");

    /*
     * The reference loader builds a payload-args blob (UDP socket pair, an
     * in-victim pipe and the kernel data base) and SDK payloads depend on it:
     * their stdio runs through those handles, so a payload started without it
     * stays alive but never reaches its own code. On by default, switchable
     * off with PSXTERM_PS5_PAYLOAD_ARGS=0.
     */
    {
        const char *args_opt = getenv("PSXTERM_PS5_PAYLOAD_ARGS");
        bool want_args = args_opt == NULL || args_opt[0] != '0';

        if(want_args && (args = build_payload_args(pid))) {
            PSX_LOGI("ps5: payload args: reference ABI at 0x%lx",
                     (unsigned long)args);
        } else {
            intptr_t page = pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

            if(page == -1) {
                PSX_LOGW("ps5: minimal payload args page unavailable (%s)",
                         strerror(errno));
                args = 0;
            } else {
                args = page;
                PSX_LOGW("ps5: payload args: falling back to a zeroed page "
                         "0x%lx",
                         (unsigned long)page);
            }
        }
    }

    PSX_LOGI("ps5: payload args: done");

    stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, 0, NULL);

    PSX_LOGI("ps5: setregs: start");

    if(pt_getregs(pid, &regs)) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "pt_getregs failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    regs.r_rsp &= ~0xfl;
    regs.r_rsp -= 8;
    regs.r_rip = entry;
    regs.r_rdi = (uint64_t)args;

    PSX_LOGI("ps5: setregs: applying, then detaching");

    if(pt_setregs(pid, &regs)) {
        stage_set(failure, PSX_SPAWN_STAGE_SET_REGISTERS, errno,
                  "pt_setregs failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    stage_set(failure, PSX_SPAWN_STAGE_DETACH, 0, NULL);

    if(pt_detach(pid, 0)) {
        stage_set(failure, PSX_SPAWN_STAGE_DETACH, errno, "pt_detach failed");
        PSX_LOGE("ps5: pt_detach: %s", strerror(errno));
        kill(pid, SIGKILL);
        free(elf);
        return -1;
    }

    free(elf);

    stage_set(failure, PSX_SPAWN_STAGE_RUNNING, 0, NULL);

    /*
     * Give the payload a moment and record whether it survived the start:
     * distinguishes "crashed immediately" from "running but its stdio is not
     * connected", which is otherwise indistinguishable from the outside.
     */
    {
        int status = 0;
        struct timespec settle = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};

        nanosleep(&settle, NULL);

        if(waitpid(pid, &status, WNOHANG) == pid) {
            PSX_LOGE("ps5: payload exited at start (status %d, exit %d, sig %d)",
                     status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                     WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        } else {
            PSX_LOGI("ps5: payload alive after start");
        }
    }

    return pid;
}

int
psx_platform_process_wait(pid_t pid, int *status_out, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? psx_now_ms() + (uint64_t)timeout_ms : 0;

    for(;;) {
        pid_t rc = waitpid(pid, status_out, timeout_ms == 0 ? WNOHANG : 0);

        if(rc == pid) {
            return 0;
        }
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(timeout_ms == 0 || (timeout_ms > 0 && psx_now_ms() >= deadline)) {
            return 1;
        }
        usleep(10000);
    }
}

int
psx_platform_process_kill(pid_t pid, int sig)
{
    return kill(pid, sig);
}

bool
psx_platform_process_available(void)
{
    /*
     * The backend depends on the SDK kernel helpers being usable in this
     * process. That is the only precondition PSXTerm can check without
     * actually spawning something.
     */
    return kernel_get_proc(getpid()) != 0;
}

const char *
psx_platform_process_name(void)
{
    return "ps5-elfldr";
}
