# PS5 payload stdio relay — design and safety

Status: DESIGN (implementation pending hardware availability)

This document exists because two physical consoles panicked while exploring
how a spawned payload's libc stdio reaches the session. The rule that came
out of it is short enough to state first, and everything else follows from it.

## The hard rule

`rwpipe` / `kpipe_addr` in the payload args **must always describe a real
pipe**. The payload runtime treats the kernel file object behind those
handles as a pipe; handing it the file object of a socket (or any other
descriptor type) makes the runtime read and write kernel structures that are
not a pipe, which corrupts kernel memory and panics the console. Both panics
happened exactly there.

A second consequence, observed the same way: the runtime **blocks reading its
input from `rwpipe[0]` while it starts**. An empty pipe is therefore safe (the
runtime waits), but anything that never writes into that pipe leaves the
payload parked before `main`.

## What is already verified on hardware

| Observation | Evidence |
|---|---|
| Raw descriptor writes reach the session | `hello.elf` writing `write(1, ...)` shows up in the session; the client receives the exit status |
| The payload reaches its entry and exits | `cli_test.elf` prints its report and exits 7, propagated as the client's exit code |
| Arguments and environment arrive intact | diagnostics: `argv[1]=doctor`, quoted argument preserved, `env PSXTERM_DOCTOR=1` |
| Stack addresses are not usable by the child | the child logged `argc=16` with an unreadable `argv[0]` while a `.rodata` string came through; fixed by copying both vectors into a heap arena |
| A private pipe inside the victim is safe | the spawn completes and payloads run with the reference args blob |
| A non-pipe descriptor in the runtime handles is fatal | two console panics |

## Design

The relay needs a pipe whose **other** end the daemon holds, so nothing about
the runtime handles changes type:

```
payload libc stdio
        │  rwpipe[0] (read)  rwpipe[1] (write)      real pipe, created by the daemon
        ▼
   victim fds (imported with rdup)
        ▼
   daemon pipe ends ── relay ── session STDOUT / STDERR / STDIN ── remote client
```

1. **Create the pipe in the daemon** (`pipe(fds)`), not inside the victim.
   A daemon-owned pipe is what makes the relay possible at all: the daemon
   already holds both ends and can pump them.
2. **Import both ends into the victim** with `pt_rdup(pid, getpid(), fd)`
   (the same call the stdio wiring already uses successfully), so the payload
   sees valid descriptors.
3. **Put the imported fds in the args**: `rwpipe[0]` = imported read end,
   `rwpipe[1]` = imported write end, and `kpipe_addr` =
   `kernel_get_proc_file(pid, imported_read_fd)` — a real pipe kernel object,
   which is what keeps the runtime on safe ground.
4. **Expose the daemon's ends to the spawn caller** (session and diagnostics)
   through `psx_spawn_options_t` / the spawn result, so the session loop can
   poll them next to the tty.
5. **Pump in the session loop**: session input → daemon write end; daemon read
   end → session output. Keep the two directions separate; do not merge
   stderr into stdout.

### Where the boundaries are

- `platform/ps5/process.c` creates the pipe, imports it, fills the args and
  returns the daemon-side fds. This is the file the hardware regression rule
  warns about: change it only with the probes green.
- `core/session.c` and `core/diag.c` own the pumping, because they already own
  the terminal side. `core/process.c` only carries the fds through.
- Nothing in this path exposes kernel primitives to clients.

## Safety procedure for the next hardware attempt

1. Enable the klog mirror to disk **before** the test
   (`klog-capture.sh`), so a failure leaves the sequence on this machine.
2. Keep the relay behind an environment switch (`PSXTERM_PS5_STDIO_RELAY=1`)
   with the private-pipe path as the default, so the daemon can always be
   redeployed into the safe configuration without touching the console.
3. First run: spawn `hello.elf` only. If the payload does not print, do not
   retry blindly - pull `/data/psxterm/child.log` and check the daemon log.
4. Only after `hello.elf` prints through the relay: run the full acceptance
   matrix below.

## Acceptance matrix (from the roadmap, unchanged)

A complex CLI must behave like a normal CLI. From an externally spawned
payload: `write(1)`, `write(2)`, `printf`, `fprintf` on both streams, `puts`,
`fgets`, `fread`, `fflush` on both streams; stdin interactive; exit status
propagated; stdout and stderr distinguishable at the client
(`PTTY_MSG_STDOUT` / `PTTY_MSG_STDERR`).

## Open questions to answer on hardware

- Does the runtime use `rwpipe[0]` for input and `rwpipe[1]` for output, or the
  other way round? The blocking behaviour at startup suggests `rwpipe[0]` is
  the input side, but this must be confirmed with the relay in place rather
  than by guessing (guessing is what caused the panics).
- What is `payloadout` for? It is a pointer to an int in the args page; watch
  the value across a run before assuming.
- Is `rwpair` (the UDP socket pair) used for anything the relay must serve, or
  only by the runtime's own internals?
