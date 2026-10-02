/*
 * PS4 process backend.
 *
 * External executables are payload ELFs injected into a freshly created
 * process, following the minimal subset of the ps4-payload-dev/elfldr
 * mechanism that PSXTerm needs (GPLv3, John Törnblom):
 *
 *   rfork_thread(RFPROC | RFCFDG | RFMEM)
 *     child: budget syscall, open /dev/deci_std{in,out,err},
 *            sched_setscheduler(SCHED_RR), ptrace(PT_TRACE_ME),
 *            execve(SceSpZeroConf, argv, envp)
 *   parent: attach, relocate the eboot, load the payload ELF into the
 *           stopped process, wire stdio to the session tty, detach.
 *
 * Remote operations use the PS4 debugger service (mdbg) for memory access
 * and a syscall trampoline patch for remote syscalls, as the reference
 * loader does.
 *
 * HARDWARE TEST REQUIRED: none of this has been executed on a PS4.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <machine/reg.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/unistd.h>
#include <sys/wait.h>

#include <ps4/kernel.h>
#include <ps4/klog.h>
#include <ps4/mdbg.h>

#include "psxterm/log.h"
#include "psxterm/process.h"
#include "psxterm/util.h"

#define PS4_EBOOT "/system/vsh/app/NPXS21016/eboot.bin"

#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x) ((x) & ~(PAGE_SIZE - 1))

#define SYS_BUDGET_SET 0x23b
#define SYS_RDUP 0x25b
#define SYS_PROCESS_NEEDED_AND_RELOCATE 0x257
#define SYS_GET_PROC_PARAM 0x256

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

    if(waitpid(pid, 0, 0) == -1) {
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
    return mdbg_copyin(pid, buf, addr, len);
}

static int
pt_copyout(pid_t pid, intptr_t addr, void *buf, size_t len)
{
    return mdbg_copyout(pid, addr, buf, len);
}

/*
 * Remote syscall: patch the instruction at the current rip with
 * `syscall` (0x0f 0x05), single step, then restore.
 */
static long
pt_syscall(pid_t pid, int sysno, ...)
{
    uint16_t sysc_instr = 0x050f;
    struct reg sysc_reg;
    struct reg bak_reg;
    uint16_t bak_instr;
    va_list ap;

    if(pt_getregs(pid, &bak_reg)) {
        return -1;
    }

    if(pt_copyout(pid, bak_reg.r_rip, &bak_instr, sizeof(bak_instr))) {
        return -1;
    }

    memcpy(&sysc_reg, &bak_reg, sizeof(sysc_reg));
    sysc_reg.r_rax = sysno;

    va_start(ap, sysno);
    sysc_reg.r_rdi = va_arg(ap, uint64_t);
    sysc_reg.r_rsi = va_arg(ap, uint64_t);
    sysc_reg.r_rdx = va_arg(ap, uint64_t);
    sysc_reg.r_r10 = va_arg(ap, uint64_t);
    sysc_reg.r_r8 = va_arg(ap, uint64_t);
    sysc_reg.r_r9 = va_arg(ap, uint64_t);
    va_end(ap);

    if(pt_setregs(pid, &sysc_reg)) {
        return -1;
    }

    if(pt_copyin(pid, &sysc_instr, sysc_reg.r_rip, sizeof(sysc_instr))) {
        return -1;
    }

    if(pt_step(pid)) {
        return -1;
    }

    if(pt_getregs(pid, &sysc_reg)) {
        return -1;
    }

    if(pt_setregs(pid, &bak_reg)) {
        return -1;
    }

    if(pt_copyin(pid, &bak_instr, bak_reg.r_rip, sizeof(bak_instr))) {
        return -1;
    }

    return sysc_reg.r_rax;
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
pt_msync(pid_t pid, intptr_t addr, size_t len, int flags)
{
    return (int)pt_syscall(pid, SYS_msync, addr, len, flags);
}

static int
pt_munmap(pid_t pid, intptr_t addr, size_t len)
{
    return (int)pt_syscall(pid, SYS_munmap, addr, len);
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
pt_dynlib_process_needed_and_relocate(pid_t pid)
{
    return (int)pt_syscall(pid, SYS_PROCESS_NEEDED_AND_RELOCATE);
}

/* ------------------------------------------------------------------ */
/* child entry used by rfork_thread                                   */
/* ------------------------------------------------------------------ */

struct ps4_spawn_context {
    char *const *argv;
    char *const *envp;
    const char *path;
};

static int
ps4_rfork_entry(void *arg)
{
    struct ps4_spawn_context *ctx = arg;
    struct sched_param sp;

    if(syscall(SYS_BUDGET_SET, 0) != 0) {
        klog_perror("psxterm: budget");
        return -1;
    }

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

    sp.sched_priority = sched_get_priority_min(SCHED_RR);
    if(sched_setscheduler(0, SCHED_RR, &sp)) {
        klog_perror("psxterm: sched_setscheduler");
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

    /* PS4 payloads are mapped writable and executable as a whole. */
    if((base_addr = pt_mmap(pid, base_addr, base_size,
                            PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1,
                            0)) == -1) {
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
        if(phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }

        if(pt_mprotect(pid, base_addr + phdr[i].p_vaddr,
                       ROUND_PG(phdr[i].p_memsz),
                       (((phdr[i].p_flags & PF_R) ? PROT_READ : 0) |
                        ((phdr[i].p_flags & PF_W) ? PROT_WRITE : 0) |
                        ((phdr[i].p_flags & PF_X) ? PROT_EXEC : 0)))) {
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

/* Give the payload's libc an unlimited heap (reference-loader behaviour). */
static void
set_heap_size(pid_t pid, int size)
{
    intptr_t buf;
    intptr_t sce_proc_param;
    intptr_t sce_libc_param;
    intptr_t sce_libc_heap_size;

    if((buf = pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
        return;
    }

    if(pt_syscall(pid, SYS_GET_PROC_PARAM, buf, buf + 0x100)) {
        pt_munmap(pid, buf, PAGE_SIZE);
        return;
    }

    if(!(sce_proc_param = mdbg_getlong(pid, buf))) {
        pt_munmap(pid, buf, PAGE_SIZE);
        return;
    }

    if(mdbg_copyout(pid, sce_proc_param + 56, &sce_libc_param,
                    sizeof(sce_libc_param)) ||
       mdbg_copyout(pid, sce_libc_param + 16, &sce_libc_heap_size,
                    sizeof(sce_libc_heap_size))) {
        pt_munmap(pid, buf, PAGE_SIZE);
        return;
    }

    mdbg_setint(pid, sce_libc_heap_size, size);

    pt_munmap(pid, buf, PAGE_SIZE);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

pid_t
psx_platform_spawn(const psx_spawn_options_t *options)
{
    struct ps4_spawn_context ctx;
    struct ps4_spawn_context *child_ctx;
    struct reg regs;
    uint8_t *elf;
    size_t elf_size = 0;
    intptr_t entry;
    uint8_t *stack;
    pid_t pid;
    pid_t mypid = getpid();
    int stdio_fd;
    int attempts;

    if(!options->stdin_fd && !options->stdout_fd) {
        errno = EINVAL;
        return -1;
    }

    if(!(elf = read_file(options->path, &elf_size))) {
        return -1;
    }

    if(elf_sanity_check(elf, elf_size) != 0) {
        free(elf);
        errno = ENOEXEC;
        return -1;
    }

    if(!(stack = malloc(2 * 1024 * 1024))) {
        free(elf);
        return -1;
    }

    if(!(child_ctx = malloc(sizeof(*child_ctx)))) {
        free(stack);
        free(elf);
        return -1;
    }

    ctx.argv = options->argv;
    ctx.envp = options->envp;
    ctx.path = PS4_EBOOT;
    *child_ctx = ctx;

    if((pid = rfork_thread(RFPROC | RFCFDG | RFMEM, stack + 2 * 1024 * 1024,
                           ps4_rfork_entry, child_ctx)) < 0) {
        free(child_ctx);
        free(stack);
        free(elf);
        return -1;
    }

    if(waitpid(pid, 0, 0) < 0) {
        PSX_LOGE("ps4: waitpid: %s", strerror(errno));
        free(child_ctx);
        free(stack);
        free(elf);
        return -1;
    }

    free(child_ctx);
    free(stack);

    for(attempts = 0; attempts < 100; attempts++) {
        if(pt_attach(pid) == 0) {
            break;
        }
        if(errno != EBUSY) {
            PSX_LOGE("ps4: pt_attach: %s", strerror(errno));
            kill(pid, SIGKILL);
            free(elf);
            return -1;
        }
        sched_yield();
    }
    if(attempts == 100) {
        PSX_LOGE("ps4: pt_attach kept returning EBUSY");
        kill(pid, SIGKILL);
        free(elf);
        errno = EBUSY;
        return -1;
    }

    if(pt_dynlib_process_needed_and_relocate(pid)) {
        PSX_LOGW("ps4: process_needed_and_relocate failed");
    }

    set_heap_size(pid, -1);

    /* Wire stdio to the session tty before starting the payload. */
    stdio_fd = pt_rdup(pid, mypid, options->stdin_fd);
    if(stdio_fd < 0) {
        PSX_LOGE("ps4: cannot duplicate the session fd: %s", strerror(errno));
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    pt_close(pid, STDIN_FILENO);
    pt_close(pid, STDOUT_FILENO);
    pt_close(pid, STDERR_FILENO);
    pt_dup2(pid, stdio_fd, STDIN_FILENO);
    pt_dup2(pid, stdio_fd, STDOUT_FILENO);
    pt_dup2(pid, stdio_fd, STDERR_FILENO);
    pt_close(pid, stdio_fd);

    if(!(entry = load_elf(pid, elf, elf_size))) {
        PSX_LOGE("ps4: ELF load failed: %s", strerror(errno));
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_getregs(pid, &regs)) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    regs.r_rsp &= ~0xfl;
    regs.r_rsp -= 8;
    regs.r_rip = entry;
    regs.r_rdi = 0;

    if(pt_setregs(pid, &regs)) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_detach(pid, 0)) {
        PSX_LOGE("ps4: pt_detach: %s", strerror(errno));
        kill(pid, SIGKILL);
        free(elf);
        return -1;
    }

    free(elf);

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
    return kernel_get_proc(getpid()) != 0;
}

const char *
psx_platform_process_name(void)
{
    return "ps4-elfldr";
}
