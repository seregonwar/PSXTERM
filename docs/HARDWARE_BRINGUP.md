# Hardware bring-up guide (PS4 / PS5)

PSXTerm is host tested, and the PS5 bring-up is now in progress. This
document records the sequence and, in the results section below, exactly what
was observed on hardware. Anything not listed there is still
`HARDWARE TEST REQUIRED`.

## Observed results

### PS5 (firmware unrecorded, payload loaded with ps5-payload-dev elfldr)

Status: **PARTIAL** - daemon, protocol, shell, diagnostics and file transfer
run on hardware; the process (exec) backend is still being validated.

| Item | Result |
|---|---|
| `psxtermd` payload loads and runs | PASS (`PSXTerm 0.1.0 (PS5) listening on 0.0.0.0:2323`) |
| PTTY/1 handshake, OPEN, prompt over TCP | PASS |
| Remote shell builtins (`pwd`, `uname`, `whoami`, `ls /`, `ls /dev`, `ls /data`) | PASS |
| Diagnostics (`psxterm doctor`) over the network | PASS (report delivered) |
| Kernel log visibility (`klog` on port 3232) | PASS (daemon logs appear as `[payload.elf] psxterm: ...`) |
| `/data` writable, sandbox lifted | PASS |
| `/data/psxterm` + `/data/psxterm/bin` | FAIL on first run: they did not exist; the daemon now creates them |
| Real FreeBSDPTY (`/dev/ptmx`) | **NOT AVAILABLE**: `/dev/ptmx` and `/dev/pts` do not exist on the console; `ls /dev` shows `deci_tty*`, `ctty`, `ttyu0`. PipeTTY is the working backend (`REAL PTY NOT YET AVAILABLE`) |
| Monotonic clock | FAIL: `clock_gettime(CLOCK_MONOTONIC)` does not advance (diagnostics elapsed time was 0 ms); `psx_now_ms()` now falls back to `CLOCK_REALTIME`/`gettimeofday` and a doctor check verifies that the clock advances |
| Sandbox escape (privileges) | FIXED, HARDWARE TESTED: the payload stayed in the Sony jail and everything downstream misbehaved. `psx_privilege_raise()` (ported from the MemDBG privilege manager) applies full caps, the system authid, uid 0 and the root/jail vnode; hardware log confirms `privilege: sandbox escaped (root vnode 0xffffc5300340a760)` |
| Instance replacement without reboot | PASS: pid file + SIGTERM/SIGKILL replaces only the previous psxtermd. Verified repeatedly, including recovering a daemon hung inside a kernel call with no console reboot |
| Spawn | PASS end to end on hardware: the whole chain completes (CREATE_VICTIM, exec stop, RELOCATE, breakpoint, DUP_STDIO, LOAD_ELF, payload args, SET_REGISTERS, DETACH) and the payload process actually executes |
| Payload execution | PASS end to end, HARDWARE VERIFIED on a second console: `hello.elf` prints through stdout, stderr and libc printf (`HELLO A..E`) and the client receives the exit status; `cli_test.elf` prints its full report (argv, environment, isatty, winsize, stdin prompt) and exits 7, which the client reports as its own exit code |
| Payload stdin | A payload that reads stdin blocks until the session delivers data and the end of input: with stdin closed it runs to completion, which is why the earlier runs looked silent (the process was parked in `fgets`). The diagnostics execution check must feed stdin and close it the same way |
| isatty on console | 0 for stdin/stdout/stderr and `winsize: unavailable`, because the console has no `/dev/ptmx`: the session falls back to the loopback pipe pair. Reported as WARN by the doctor, not a failure |
| Console incident | Two consoles panicked, both times on the same experiment: pointing the payload runtime's stdio handles (`rwpipe` / `kpipe_addr`) at descriptors that are not a real pipe. The runtime treats the kernel file object behind those handles as a pipe, so a socket's file object there corrupts kernel memory. HARD RULE: those handles must always describe a real pipe created for the victim; the payload runtime also blocks reading its input from `rwpipe[0]` at startup, so an empty pipe (or a relay that feeds it) is the only safe shape |
| Payload stdio | PARTIAL: raw descriptor writes reach the session and are visible, and on the second console libc `printf`/`fprintf` also appeared. The runtime-free configuration is the only safe one until the pipe relay exists: the daemon must own the other end of a real pipe and pump it, never hand the runtime something else |
| Startup notifications | `PSXTERM by SeregonWar started` on start, then `PSXTERM: ...` lines for the essentials only: sandbox escape, instance replacement, `started on port <port> (platform, protocol, version)`, session connect/attach/close, shutdown. Emitted to stdout and mirrored to klog; nothing per-frame or per-byte |
| `pt_call` argument addresses | FIXED, HARDWARE VERIFIED: the pipe call was handed the daemon's own buffer address (`&pipe_fds`), so the victim wrote nowhere and the single-step loop never unwound, which is what stalled the whole spawn. The buffer must be a victim address (`buf + 0x400`) and the result is read back with `pt_copyout`. The reference ABI now builds (`args: pipe 5,6`) |
| Payload args (elfldr ABI) | Built by default and REQUIRED for payloads to work: an SDK payload handed only a zeroed page stays alive but never reaches its own code. Switchable off with `PSXTERM_PS5_PAYLOAD_ARGS=0` for experiments |
| Victim stdio | The child-side install is overwritten by the Sony eboot, so the parent-side `dup2`/`rdup` after the eboot has started is what sticks; the loopback TCP pair (rather than an AF_UNIX socketpair) is the descriptor type this kernel accepts. The parent-side install now succeeds with no "skipped" line in the log |
| Victim privilege raise | SAFE when done before the exec stop (hardware-verified, no hang); raising after the ptrace exec stop hangs the victim. The spawn now does it before waiting, matching the reference privilege manager's elevate-before-attach order |
| Load sequence stall | CLOSED: the stall was `build_payload_args` (UDP socket pair + in-victim pipe), not the segment copies. Instrumented with klog checkpoints ("load_elf: start/done", "payload args: start/done", "setregs: ..."), which is how the exact call was identified |
| Resize / signals on hardware | PENDING: unblocked by working execution, to be re-checked with the doctor now that payloads run |
| Console availability | The console became unreachable at the end of this cycle (elfldr :9021 and the daemon port both "no route to host") after the experiment that pointed the payload runtime's stdio handles at 0/1 left a payload blocked on a session read. A power cycle clears it; the instance manager covers the software side and never needs one |

The first doctor run reported `NOT READY` with these findings - exactly the
kind of stage-level information the diagnostics were built for.

## Status vocabulary

| State | Meaning |
|---|---|
| IMPLEMENTED | Code exists and builds |
| HOST TESTED | Executed successfully on the development host |
| BUILDS FOR PS4 / PS5 | Cross-compiles against the payload SDK |
| HARDWARE TEST REQUIRED | No console has executed it |
| HARDWARE TESTED | Executed successfully on a physical console |

Only promote to `HARDWARE TESTED` after the step actually succeeded on that
console model. A PS5 success does not promote PS4, and vice versa.

## 1. Prerequisites

* A payload SDK installation:
  * PS5: <https://github.com/ps5-payload-dev/sdk> (prebuilt release works)
  * PS4: <https://github.com/ps4-payload-dev/sdk>
* LLVM/clang 18 or newer with `lld` on `PATH` (the SDK wrappers locate
  `llvm-config-18` … `llvm-config-15`).
* An existing payload loader for your console (the SDK README lists known
  working ones). PSXTerm does not require a new loader protocol; ship
  `psxtermd` the same way you ship any other payload.
* A PC on the same network running the `psxterm` client.

## 2. Build

Host (for the client and for reference behaviour):

```console
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
```

PS5 payloads:

```console
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cmake -S . -B build-ps5 -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake
cmake --build build-ps5 -j
```

PS4 payloads:

```console
export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_PAYLOAD_SDK/toolchain/orbis.cmake
cmake --build build-ps4 -j
```

Expected artifacts in `build-ps5/` or `build-ps4/`:

| Artifact | Purpose |
|---|---|
| `psxtermd` | the daemon payload (default port 2323) |
| `psxterm-ttyprobe` | standalone TTY capability probe |
| `cli_test.elf` | controlled external-execution target (exit code 7) |

Target tools live under `/data/psxterm/bin` on the console. Copy
`cli_test.elf` there before the process-execution checks can pass; the daemon
looks for `cli_test` or `cli_test.elf` in `/data/psxterm/bin`, next to the
running executable, and in the current directory.

## 3. Deploy and start

1. Deploy `psxtermd` with your existing loader.
2. Start it with the defaults:

   ```console
   psxtermd                 # port 2323, authentication disabled (development)
   psxtermd --token SECRET  # shared-token authentication
   psxtermd --tty pipe      # force the PipeTTY fallback for comparison
   ```

3. Watch the daemon log (send it to the loader's console/serial output):

   ```
   psxterm: info: PSXTerm 0.1.0 (PS5) listening on 0.0.0.0:2323
   psxterm: warn: authentication: DISABLED (insecure development mode) - ...
   psxterm: info: tty: FreeBSDPTY backend available (pts 2)      <- or:
   psxterm: warn: tty: real PTY not available, using PipeTTY fallback (reason)
   ```

   Whichever of those two lines appears is the first real hardware finding.

## 4. Diagnostic sequence

Run the steps in order; each one isolates a layer.

### Step 1 - local TTY probe

```console
psxterm-ttyprobe
```

Exit status `0` means the FreeBSDPTY backend is usable, `1` means only the
PipeTTY fallback is available. Record the structured output verbatim.

### Step 2 - local diagnostics (no network)

```console
psxtermd --doctor          # human readable
psxtermd --doctor --json   # machine readable
```

This runs platform, filesystem, TTY, session (skipped locally) and process
checks. It exercises `cli_test` through the real spawn backend three times, so
it answers "would a terminal session work on this console?" without a client.

Exit status: `0` ready, `1` ready with warnings, `2` not ready.

### Step 3 - connect from the PC

```console
psxterm 192.168.1.50            # interactive session
psxterm 192.168.1.50 --token SECRET
```

Expected: `Connected to PlayStation 5` / `PSXTerm 0.1.0` followed by a
`ps5:~ $` prompt.

### Step 4 - remote diagnostics

```console
psxterm doctor 192.168.1.50               # human readable
psxterm doctor --json 192.168.1.50 > doctor.json
```

The client first performs a PING/PONG roundtrip, then streams the report. The
report contains a `Session` group (socket, TCP_NODELAY, frame counters) that
the local mode cannot provide. Attach `doctor.json` to any issue report.

### Step 5 - external execution

```console
psxterm 192.168.1.50
ps5:~ $ /data/psxterm/bin/cli_test alpha "beta gamma"
```

Expected: argv, environment, `isatty` results, the stdin line read back, a
message on stderr, and exit status `7` (reported as `[exit 7]`).

### Step 6 - remote workflows

Once a session works, the transport features can be exercised too:

```console
psxterm push 192.168.1.50 ./cli_test.elf /data/psxterm/bin/cli_test.elf
psxterm sessions 192.168.1.50                 # running/detached sessions
# Ctrl+] inside a session detaches it; the shell keeps running
psxterm attach 192.168.1.50 <id> --resume <token>
psxterm pull 192.168.1.50 /data/psxterm/log.txt ./log.txt
```

If a push reports a size mismatch, no partial file is published: the daemon
removes its temporary upload and reports the error.

## 5. Failure triage

Keep bring-up surgical: fix the layer the report points at, not the whole
stack.

| Report says | Look at | Do not touch |
|---|---|---|
| `/dev/ptmx` FAIL | ptmx permissions/availability, loader sandbox | process loading |
| `TIOCGPTN` FAIL | PTY availability on this firmware | ELF loading |
| PTY PASS, `spawn` FAIL at `ATTACH` | ptrace attach flow | ELF relocation |
| `spawn` FAIL at `LOAD_ELF` / `RELOCATE` | ELF mapping, relocations, mprotect | TTY handling |
| `spawn` FAIL at `DUP_STDIO` | fd duplication (`pt_rdup`/`dup2`) | ELF loader |
| spawn PASS, `stdout` PASS, `stdin` FAIL | stdio/TTY ownership and echo | protocol framing |
| `SIGINT` FAIL | signal delivery to the foreground process | shell code |
| everything PASS locally, remote FAIL | networking/auth | console runtime |

The spawn stage names come from the backend itself
(`PREPARE`, `CREATE_VICTIM`, `ATTACH`, `RAISE_PRIVILEGES`, `DUP_STDIO`,
`LOAD_ELF`, `RELOCATE`, `SET_REGISTERS`, `DETACH`, `RUNNING`), so a failure
tells you exactly where it stopped.

## 6. What to collect on failure

1. Console model and firmware, loader name/version.
2. The `psxtermd -v` log from startup up to the failure.
3. `psxterm-ttyprobe` output (local).
4. `psxtermd --doctor` output, ideally `--json`.
5. For remote failures: `psxterm doctor --json <host>` output and whether the
   interactive session worked at all.
6. Whether the console rebooted, hung, or kept running.

## 7. Known limitations of the diagnostics

* The daemon is single threaded: `doctor` runs synchronously and briefly
  blocks other sessions (the process checks take a few hundred milliseconds).
* Diagnostics refuse to run while a foreground process is active
  (status `2` with an explanatory line) so they never disturb a running
  program.
* Console firmware is reported as `UNKNOWN`: there is no reliable payload-side
  interface for it, and a guessed value would be worse than an honest UNKNOWN.
* `stdio separation` is validated by running `cli_test` a second time with
  stderr on its own descriptor; if the backend rejects distinct descriptors,
  this check fails even though terminal sessions still work.
