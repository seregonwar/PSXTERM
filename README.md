# PSXTerm

Remote interactive terminal environment for exploited **PlayStation 4** and
**PlayStation 5** systems.

```
PC / Mac / Linux terminal          PS4 / PS5
        psxterm  <host>            psxtermd (payload)
             |                          |
             |        TCP :2323         |
             +-------- PTTY/1 ----------+
                                        |
                                   session tty
                                  (FreeBSDPTY or
                                     PipeTTY)
                                        |
                                      psh
                                        |
                       +----------------+----------------+
                       |                                 |
                 built-in commands               external CLI ELFs
             (help, pwd, cd, ls, cat, ...)     (/data/psxterm/bin/*)
```

PSXTerm does **not** require Bash, OpenSSH or any desktop userspace on the
console. It ships its own minimal shell, `psh`, and its own framed protocol,
`PTTY/1`.

## Status

Terminology follows the project's strict rules:

| Component | Status |
|---|---|
| PTTY/1 protocol (framing, validation, partial IO) | IMPLEMENTED, HOST TESTED |
| psxtermd server (poll loop, sessions, timeouts, limits) | IMPLEMENTED, HOST TESTED |
| `psh` shell + builtins | IMPLEMENTED, HOST TESTED |
| Host client (raw mode, resize, terminal restore, batch mode) | IMPLEMENTED, HOST TESTED |
| FreeBSDPTY backend (host POSIX ptmx) | IMPLEMENTED, HOST TESTED |
| PipeTTY fallback | IMPLEMENTED, HOST TESTED |
| External execution (host fork/exec) | IMPLEMENTED, HOST TESTED |
| FreeBSDPTY backend (raw /dev/ptmx + TIOCGPTN) | IMPLEMENTED, BUILD FOR PS4/PS5, HARDWARE TEST REQUIRED |
| External execution on PS4/PS5 (elfldr-style) | IMPLEMENTED, BUILDS FOR PS4/PS5, HARDWARE TEST REQUIRED |
| PS4/PS5 filesystem preparation | IMPLEMENTED, BUILDS FOR PS4/PS5, HARDWARE TEST REQUIRED |
| Structured diagnostics + spawn stages | IMPLEMENTED, HOST TESTED |
| `psxterm doctor` (local and remote, `--json`) | IMPLEMENTED, HOST TESTED |
| Capability negotiation (`PTTY_MSG_CAPS`) | IMPLEMENTED, HOST TESTED |
| Backpressure bounds (input + output queues) | IMPLEMENTED, HOST TESTED |
| Persistent sessions (detach / attach / resume, bounded scrollback) | IMPLEMENTED, HOST TESTED |
| File transfer (push / pull / install, atomic uploads) | IMPLEMENTED, HOST TESTED |
| Runtime layout, environment and package metadata | IMPLEMENTED, HOST TESTED; updated runtime sources compile for PS4/PS5, HARDWARE TEST REQUIRED |
| Challenge-response authentication | NOT IMPLEMENTED (shared token only) |
| psh history / completion / cursor editing | NOT IMPLEMENTED |
| psh pipelines and redirection | NOT IMPLEMENTED |
| Job control (jobs/fg/bg) | NOT IMPLEMENTED |
| Hardware bring-up guide | WRITTEN ([docs/HARDWARE_BRINGUP.md](docs/HARDWARE_BRINGUP.md)) |
| CI: host build/tests, ASan/UBSan, PS4/PS5 SDK payload jobs | IMPLEMENTED |
| Client on Windows | UNSUPPORTED (protocol layer is portable) |
| Authentication | Architecture + shared-token mode IMPLEMENTED; disabled by default (insecure development mode) |

The PS4/PS5 builds produce payload ELFs with the official
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) and
[ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk) SDKs. PS5 bring-up
has reached physical hardware, with partial results recorded in
[docs/HARDWARE_BRINGUP.md](docs/HARDWARE_BRINGUP.md); PS4 hardware validation
remains pending. Do not read "builds" as "works".

## Layout

```
include/psxterm/   public headers (protocol, server, session, tty, process, shell, platform)
core/              protocol, server, session, tty abstraction, process abstraction
shell/             psh: parser, environment, command registry, builtins/
platform/host/     host backend (Linux/macOS): POSIX PTY, fork/exec
platform/ps4/      PS4 backend: raw ptmx, elfldr-style spawn, fs prep
platform/ps5/      PS5 backend: raw ptmx, elfldr-style spawn, fs prep
client/            psxterm host client
tools/             psxterm-ttyprobe, cli_test
tests/             host unit tests (CTest) and tests/integration (Python)
```

## Building

Host (development, Linux/macOS/WSL):

```console
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
python3 tests/integration/test_e2e.py --build build-host
```

PS5 / PS4 payloads, using a payload SDK installation:

```console
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cmake -S . -B build-ps5 -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake
cmake --build build-ps5 -j
# -> build-ps5/psxtermd  build-ps5/psxterm-ttyprobe  build-ps5/cli_test.elf

export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_PAYLOAD_SDK/toolchain/orbis.cmake
cmake --build build-ps4 -j
```

## Running

On the console (payload), after loading `psxtermd` with an ELF loader:

```console
psxtermd                       # port 2323, authentication disabled (insecure)
psxtermd -p 2323 --token SECRET
psxtermd --max-sessions 4 --handshake-timeout 5000 --idle-timeout 600000
psxtermd --tty auto|pty|pipe   # force a tty backend (debugging)
psxtermd --doctor [--json]     # local diagnostics, no client needed
```

From the PC:

```console
psxterm 192.168.1.50                    # interactive session
psxterm 192.168.1.50 -p 2323 -t SECRET
psxterm 192.168.1.50 -e "uname"         # run one command, exit with its status
psxterm doctor 192.168.1.50             # remote diagnostics
psxterm doctor --json 192.168.1.50      # machine-readable report
psxterm sessions 192.168.1.50           # list sessions (running/detached)
psxterm attach 192.168.1.50 3 --resume <token>
psxterm push 192.168.1.50 ./tool.elf /data/psxterm/bin/tool.elf
psxterm pull 192.168.1.50 /data/psxterm/log.txt ./log.txt
psxterm install 192.168.1.50 ./tool.elf  # -> /data/psxterm/bin/tool.elf
```

Inside a session, **Ctrl+]** detaches: the shell, its cwd, environment and
foreground process keep running, output is buffered in a bounded scrollback
(oldest bytes dropped, reported on reattach), and `attach --resume` resumes it
with the session's resume token. A dropped connection behaves like a detach;
`--no-persist` restores destroy-on-disconnect.

`doctor` pings the daemon, then reports platform, filesystem, TTY, session and
process-execution checks, running the controlled `cli_test` target through the
real spawn backend. Exit codes: `0` ready, `1` ready with warnings, `2` not
ready, `64` invalid client usage. See
[docs/HARDWARE_BRINGUP.md](docs/HARDWARE_BRINGUP.md) for the full console
bring-up sequence and failure triage.

### psh

Built-ins: `help pwd cd ls cat clear env export unset uname whoami ps exit`.
Anything else is resolved through `PATH` (default `/data/psxterm/bin`, with an
implicit `.elf` suffix on consoles) and executed as an external ELF on the
session tty.

External commands receive the runtime home, temporary and XDG directories,
certificate paths and a runtime-first `PATH`. See
[docs/RUNTIME.md](docs/RUNTIME.md) for the layout, metadata lifecycle and
validation status.

### TTY capability probe

```console
psxterm-ttyprobe
```

It opens `/dev/ptmx`, calls `TIOCGPTN`, opens the corresponding `/dev/pts/<n>`,
checks termios, window-size ioctls and `isatty()`, and performs a master/slave
roundtrip. Exit status 0 means FreeBSDPTY is usable; 1 means only the PipeTTY
fallback is available. On consoles this is the runtime check that decides
whether `FreeBSDPTY` is enabled - headers alone prove nothing.

## Protocol

`PTTY/1` is a small framed protocol: a fixed 16-byte little-endian header
(magic `PTTY`, version, type, flags, session id, payload length) followed by an
optional payload of at most 64 KiB. Frames are validated (magic, version,
length, session id) and parsed field by field - no struct casting over
network buffers. Messages: `HELLO HELLO_ACK OPEN OPEN_OK CLOSE STDIN STDOUT
STDERR RESIZE SIGNAL EXEC EXIT PING PONG`. See `include/psxterm/protocol.h`.

## Security

Authentication is **disabled by default** and that is clearly logged as
insecure development mode. Use `--token` for a shared-secret handshake. Never
expose the daemon to untrusted networks while authentication is disabled.

## Testing

* 9 host unit suites via CTest (protocol, parser, env, registry, session,
  tty, shell, diagnostics, runtime).
* 45 host integration tests driving a real daemon with an independent Python
  implementation of PTTY/1, covering malformed input, authentication, session
  limits, resize, signals, multi-session isolation, timeouts, the doctor
  command (human, JSON, busy session, client binary), capabilities,
  backpressure floods with bounded-RSS assertions, detach/attach/resume with
  scrollback truncation, file transfer (roundtrip, atomicity, path safety,
  client binary), runtime metadata JSON escaping and restart preservation,
  manifest failure reporting and a disconnect/fd-leak stress loop.
* The host PipeTTY stdio matrix checks live `fgets`, binary `fread`, output
  after stdin EOF, separate stdout/stderr channels, the real client, session
  reuse, descriptor cleanup and detached stderr. See
  [docs/STDIO_HOST.md](docs/STDIO_HOST.md) for the scope and console limitation.
* `cli_test` is the controlled external-execution target (argv, environment,
  `isatty`, window size, stdin, stdout/stderr, exit code 7).

## License

GPLv3. The PS4/PS5 process backends follow the minimal concepts of the
GPLv3 [ps4/ps5-payload-dev](https://github.com/ps5-payload-dev) ELF loaders.
