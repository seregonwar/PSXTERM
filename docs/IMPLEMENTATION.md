# PSXTerm implementation status

This document is the Milestone 0 audit plus the milestone-by-milestone status
required by the development prompt. Status vocabulary: IMPLEMENTED, HOST
TESTED, BUILDS FOR PS4, BUILDS FOR PS5, HARDWARE TEST REQUIRED, HARDWARE
TESTED, UNSUPPORTED.

## Milestone 0 - Repository audit

The repository contained only `README.md`, `LICENSE` (GPLv3), `.gitignore`,
`.gitattributes` and an untracked development prompt. There was no existing
architecture to preserve, so the target layout from the prompt could be
followed directly (with `platform/host` added as the development backend).

Reference inspection (read-only, nothing copied into the tree):

* `ps5-payload-dev/elfldr` - spawn flow: `rfork_thread(RFPROC|RFCFDG|RFMEM)`
  child opens `/dev/deci_std{in,out,err}`, `ptrace(PT_TRACE_ME)`,
  `execve(SceSpZeroConf, argv, envp)`; parent attaches, relocates, loads the
  ELF, builds the payload argument block, dup2s stdio, detaches.
* `ps4-payload-dev/elfldr` - same shape with `mdbg` for remote memory access,
  a `syscall` trampoline patch for remote syscalls, `pt_rdup` for fd
  injection, no payload argument block.
* SDKs - both ship prebuilt distributions with `prospero.cmake`/`orbis.cmake`
  CMake toolchains. The SDK libc exposes `posix_openpt`, `isatty`,
  `tcgetattr`, `tcsetattr`, `ioctl`, `rfork_thread`, ptrace and kqueue, but
  **no libutil**: `openpty`/`ptsname` are declarations only, so the
  `/dev/ptmx` + `TIOCGPTN` + `/dev/pts/<n>` sequence is required.

No hardware was available during development, so every console runtime path
is marked HARDWARE TEST REQUIRED.

## Milestone 1 - TTY capability probe

IMPLEMENTED, HOST TESTED. `psxterm-ttyprobe` reports structured results:
ptmx open, `TIOCGPTN`, slave open, termios, window-size ioctl, `isatty`,
master/slave roundtrip, and the resulting backend decision. It never aborts on failure and distinguishes "real PTY not yet
available" from success. PS4 and PS5 probes are independent implementations.

BUILDS FOR PS4, BUILDS FOR PS5. The probe result on hardware is unknown
(HARDWARE TEST REQUIRED for both consoles).

## Milestone 2 - Basic TCP transport

IMPLEMENTED, HOST TESTED. Socket/bind/listen/accept/recv/send in
`core/server.c`, with `SO_REUSEADDR`, `TCP_NODELAY`, non-blocking descriptors,
poll-based event loop and clean disconnect handling. Integration tests cover
connect/handshake/disconnect.

## Milestone 3 - PTTY/1 framing

IMPLEMENTED, HOST TESTED. 16-byte little-endian header, explicit
encode/decode, magic/version/length/session-id validation, 64 KiB maximum
payload, incremental reader with partial header/payload support, clean EOF,
protocol-error reporting, and an output queue that survives partial writes.
Unit tests: `tests/test_protocol.c` (105 checks). Integration tests:
oversized frame, bad magic, bad version, mismatched session id, partial
writes, truncated frames.

## Milestone 4 - Session abstraction

IMPLEMENTED, HOST TESTED. `psx_session_t` owns id, tty, environment, cwd,
dimensions, output/input queues, foreground process and state; a session
manager supports multiple sessions with unique ids and a hard limit. Unit
tests: `tests/test_session.c` (paths, manager, limits).

## Milestone 5 - Minimal psh

IMPLEMENTED, HOST TESTED. Parser (words, double quotes, escapes), command
registry and builtins `help pwd cd ls cat clear env export unset uname whoami
ps exit`. Line editing handles echo, backspace, Ctrl+C, Ctrl+D and consumes
ANSI sequences. Interactive and batch end-to-end paths are covered by unit
and integration tests.

## Milestone 6 - Real PTY backend

IMPLEMENTED, HOST TESTED (host POSIX `posix_openpt` route), BUILDS FOR PS4/PS5
(raw `/dev/ptmx` + `TIOCGPTN` + `/dev/pts/<n>` route), HARDWARE TEST REQUIRED
on both consoles. PipeTTY (socketpair) remains the fallback and is also HOST
TESTED, including `isatty() == false`, stdin EOF delivery and
Ctrl+C-to-SIGINT translation.

## Milestone 7 - Resize support

IMPLEMENTED, HOST TESTED. Client `SIGWINCH` -> `RESIZE` -> `TIOCSWINSZ`;
integration test resizes to 40x100 and observes `winsize: rows=40 cols=100`
from a spawned process; the client under a real pty forwards its terminal
size (33x123 verified). Repeated resizing is exercised by the test suite.
HARDWARE TEST REQUIRED on consoles.

## Milestone 8 - External ELF test CLI

IMPLEMENTED, HOST TESTED with `fork`/`execve`; BUILDS FOR PS4/PS5 as a payload
ELF. `tools/cli_test.c` prints argv, environment, `isatty()` per descriptor,
window size, reads a line from stdin, writes stdout/stderr and exits 7.
`psx_spawn()` is the generic API; platform internals stay under
`platform/<plat>/process.c`. HARDWARE TEST REQUIRED for PS4/PS5 spawn.

## Milestone 9 - PATH execution

IMPLEMENTED, HOST TESTED. Commands that are not built-ins are resolved through
`PATH` (with an implicit `.elf` suffix fallback) and executed on the session
tty; the exit status is reported to the client as an `EXIT` frame. Shell
expansion, pipes and redirection are intentionally absent.

## Milestone 10 - Signals

IMPLEMENTED, HOST TESTED for the minimal contract: Ctrl+C reaches a real PTY's
foreground process group through the kernel line discipline; with PipeTTY the
server translates Ctrl+C (0x03) into `SIGINT`, and the protocol `SIGNAL`
message delivers explicit signals. Ctrl+D is EOF for a foreground process and
end-of-input for the shell when idle. Ctrl+Z/job control is not implemented.

## Milestone 11 - Multiple sessions

IMPLEMENTED, HOST TESTED. Independent session ids, per-session environments
and ttys; integration tests verify isolation (no cross-session output or
environment leakage) and `ps` reporting.

## Milestone 12 - Hardening

IMPLEMENTED (host): shared-token authentication with constant-time comparison,
maximum session count with an explicit busy ACK, handshake and optional idle
timeouts, bounded output queues, malformed-frame rejection, disconnect and fd
leak stress tests (`/proc/<pid>/fd` stability over 30 connect/disconnect
cycles). Development mode stays explicitly insecure and logs a warning.

## Build and test evidence

* Host: `cmake --build build-host`, `ctest` -> 7/7 suites pass;
  `python3 tests/integration/test_e2e.py` -> 20/20 tests pass.
* PS5: `prospero.cmake` build -> `psxtermd`, `psxterm-ttyprobe`,
  `cli_test.elf` (BUILDS FOR PS5).
* PS4: `orbis.cmake` build -> `psxtermd`, `psxterm-ttyprobe`, `cli_test.elf`
  (BUILDS FOR PS4).

## Known limitations

* No console has run any PSXTerm code: every PS4/PS5 runtime path is
  HARDWARE TEST REQUIRED, including the PTY probe, filesystem preparation and
  the elfldr-based spawn.
* External execution availability is gated at runtime on the SDK kernel
  helpers; when they are unusable the shell reports it instead of failing
  silently.
* The Windows client is not implemented; the protocol and session layers are
  platform-neutral, the client is POSIX-only.
* psh has no job control, pipes, redirection or globbing by design.
