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
#include <stdlib.h>
#include <string.h>
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

static long
pt_getlong(pid_t pid, intptr_t addr)
{
    long val = 0;

    pt_copyout(pid, addr, &val, sizeof(val));

    return val;
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

    while(jmp_reg.r_rsp <= bak_reg.r_rsp) {
        if(pt_step(pid)) {
            return -1;
        }
        if(pt_getregs(pid, &jmp_reg)) {
            return -1;
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

    while(jmp_reg.r_rsp <= bak_reg.r_rsp) {
        if(pt_step(pid)) {
            return -1;
        }
        if(pt_getregs(pid, &jmp_reg)) {
            return -1;
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
};

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
        return 0;
    }

    if((master_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        return 0;
    }

    pt_setint(pid, buf + 0x00, 20);
    pt_setint(pid, buf + 0x04, IPPROTO_IPV6);
    pt_setint(pid, buf + 0x08, IPV6_TCLASS);
    pt_setint(pid, buf + 0x0c, 0);
    pt_setint(pid, buf + 0x10, 0);
    pt_setint(pid, buf + 0x14, 0);

    if(pt_setsockopt(pid, master_sock, IPPROTO_IPV6, IPV6_2292PKTOPTIONS, buf,
                     24)) {
        return 0;
    }

    if((victim_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        return 0;
    }

    pt_setint(pid, buf + 0x00, 0);
    pt_setint(pid, buf + 0x04, 0);
    pt_setint(pid, buf + 0x08, 0);
    pt_setint(pid, buf + 0x0c, 0);
    pt_setint(pid, buf + 0x10, 0);

    if(pt_setsockopt(pid, victim_sock, IPPROTO_IPV6, IPV6_PKTINFO, buf, 20)) {
        return 0;
    }

    if(kernel_overlap_sockets(pid, master_sock, victim_sock)) {
        return 0;
    }

    if(pt_call(pid, pt_resolve(pid, NID_PIPE), (uint64_t)(uintptr_t)&pipe_fds) !=
       0) {
        return 0;
    }

    kpipe_addr = kernel_get_proc_file(pid, pipe_fds[0]);
    if(!kpipe_addr) {
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

pid_t
psx_platform_spawn(const psx_spawn_options_t *options)
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
    void *stack;
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

    if(!(stack = malloc(PAGE_SIZE))) {
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
    ctx.path = PS5_EBOOT;
    *child_ctx = ctx;

    if((pid = rfork_thread(RFPROC | RFCFDG | RFMEM, stack + PAGE_SIZE - 8,
                           ps5_rfork_entry, child_ctx)) < 0) {
        free(child_ctx);
        free(stack);
        free(elf);
        return -1;
    }

    /* Wait for the child to reach the exec stop. */
    if(waitpid(pid, 0, 0) < 0) {
        PSX_LOGE("ps5: waitpid: %s", strerror(errno));
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
            PSX_LOGE("ps5: pt_attach: %s", strerror(errno));
            kill(pid, SIGKILL);
            free(elf);
            return -1;
        }
        sched_yield();
    }
    if(attempts == 100) {
        PSX_LOGE("ps5: pt_attach kept returning EBUSY");
        kill(pid, SIGKILL);
        free(elf);
        errno = EBUSY;
        return -1;
    }

    /* Let the kernel assign process parameters to the eboot. */
    if(pt_syscall(pid, SYS_PROCESS_NEEDED_AND_RELOCATE)) {
        PSX_LOGW("ps5: process_needed_and_relocate failed");
    }

    set_heap_size(pid, -1);

    /* Step past the eboot's own startup (libc initialisation). */
    if(!(brkpoint = kernel_dynlib_entry_addr(pid, 0))) {
        PSX_LOGE("ps5: cannot locate the eboot entry");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }
    brkpoint += 58;

    if(kernel_mprotect(pid, brkpoint, PAGE_SIZE,
                       PROT_READ | PROT_WRITE | PROT_EXEC)) {
        PSX_LOGE("ps5: mprotect(entry) failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_copyout(pid, brkpoint, &original_byte, sizeof(original_byte)) ||
       pt_copyin(pid, &int3, brkpoint, sizeof(int3))) {
        PSX_LOGE("ps5: cannot set the entry breakpoint");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_continue(pid, SIGCONT) || waitpid(pid, 0, 0) == -1) {
        PSX_LOGE("ps5: continue to entry failed");
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_copyin(pid, &original_byte, brkpoint, sizeof(original_byte))) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    /* Wire stdio to the session tty before starting the payload. */
    stdio_fd = pt_rdup(pid, mypid, options->stdin_fd);
    if(stdio_fd < 0) {
        PSX_LOGE("ps5: cannot duplicate the session fd: %s", strerror(errno));
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
        PSX_LOGE("ps5: ELF load failed: %s", strerror(errno));
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(!(args = build_payload_args(pid))) {
        PSX_LOGW("ps5: payload arguments unavailable, starting without them");
    }

    if(pt_getregs(pid, &regs)) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    regs.r_rsp &= ~0xfl;
    regs.r_rsp -= 8;
    regs.r_rip = entry;
    regs.r_rdi = (uint64_t)args;

    if(pt_setregs(pid, &regs)) {
        pt_detach(pid, SIGKILL);
        free(elf);
        return -1;
    }

    if(pt_detach(pid, 0)) {
        PSX_LOGE("ps5: pt_detach: %s", strerror(errno));
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
