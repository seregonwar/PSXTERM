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

## Bring-up phase (hardware validation preparation)

Ordered by the hardware bring-up plan; nothing below claims console results.

* **Structured diagnostics core** (`include/psxterm/diag.h`, `core/diag.c`):
  IMPLEMENTED, HOST TESTED. Machine-readable checks with
  PASS/FAIL/WARN/SKIP/UNKNOWN, group bookkeeping, overall result, and one
  presentation layer for human and JSON output.
* **Diagnostic checks**: IMPLEMENTED, HOST TESTED. Platform, filesystem
  (unique temp-file roundtrip), TTY (reuses the runtime probe), session/socket
  (socket endpoints, TCP_NODELAY, frame counters) and process execution.
  The process checks run the controlled `cli_test` target through the real
  backend three times to validate spawn, argv, quoting, environment, stdin,
  stdout, stderr, isatty, resize, exit status, stdout/stderr separation and
  SIGINT delivery.
* **Spawn stage reporting**: IMPLEMENTED, HOST TESTED on the host backend,
  BUILDS FOR PS4/PS5. `psx_spawn_ex()` reports the failing stage
  (PREPARE, CREATE_VICTIM, ATTACH, RAISE_PRIVILEGES, DUP_STDIO, LOAD_ELF,
  RELOCATE, SET_REGISTERS, DETACH, RUNNING), the errno and a short detail.
  Stage semantics on consoles are HARDWARE TEST REQUIRED.
* **`psxtermd --doctor [--json]`** (local, no client): IMPLEMENTED, HOST TESTED.
* **`psxterm doctor [--json] <host>`** (PING/PONG liveness roundtrip, new
  DIAG_REQUEST/DIAG_DATA/DIAG_DONE frames, exit 0/1/2): IMPLEMENTED, HOST
  TESTED. The console path is HARDWARE TEST REQUIRED.
* **Hardware bring-up guide** (`docs/HARDWARE_BRINGUP.md`): written, with the
  expected diagnostic sequence, failure triage table and log collection list.
  It deliberately contains no hardware results.
* **CI**: host matrix (Linux/macOS) with unit tests, TTY probe, integration
  suite, ASan/UBSan jobs and payload jobs that build with the official
  PS4/PS5 SDK releases; docs and release workflows. Host jobs are exercised
  locally, GitHub-hosted runs are pending the first push.
* **Capability negotiation**: IMPLEMENTED, HOST TESTED. `PTTY_MSG_CAPS`
  advertises REAL_PTY/PIPE_TTY/EXEC/JOB_CONTROL/JSON_DIAGNOSTICS/
  SESSION_RESUME/FILE_TRANSFER from runtime state; nothing is advertised that
  the process cannot actually do (EXEC follows the process backend,
  JOB_CONTROL requires a real PTY, COMPRESSION is never advertised).
* **Backpressure**: IMPLEMENTED, HOST TESTED. Output bound re-checked per
  chunk; new input bound with a high watermark that stops socket reads so TCP
  backpressure reaches the client; queued input for a dead process is
  discarded and counted. Integration tests assert bounded daemon RSS under
  input and output floods.
* **Persistent sessions**: IMPLEMENTED, HOST TESTED. DETACHED state,
  DETACH/ATTACH/ATTACH_OK/ATTACH_FAIL/SESSION_INFO, random resume tokens
  (constant-time compared, never logged), bounded 256 KiB scrollback with an
  explicit truncation notice, SESSIONS list, `--detached-timeout` reclamation
  and `--no-persist`.
* **File transfer**: IMPLEMENTED, HOST TESTED. FILE_OPEN/OPEN_OK/DATA/SEEK/
  CLOSE/RESULT/STAT, streaming in both directions, uploads through a unique
  temporary file with size validation, fsync and atomic rename, cleanup on
  every failure path, and defensive path parsing (empty, embedded NUL,
  oversized). `psxterm push|pull|install`.

Not yet implemented (next phases, in the plan's order): challenge-response
authentication, psh history/completion/cursor editing, job control, pipelines
and redirection, and fuzzing targets for the new parsers.

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
* The event loop uses `poll()` rather than kqueue/kevent. The server section of
  the development prompt allows a straightforward event loop first
  ("correctness comes before cleverness"); a kqueue backend remains future
  work and is not required for hardware bring-up.
* `doctor` runs synchronously in the single-threaded daemon and briefly blocks
  other sessions; it refuses to run while a foreground process is active.
* Console firmware cannot be discovered through a reliable payload-side
  interface, so diagnostics report it as UNKNOWN instead of guessing.
