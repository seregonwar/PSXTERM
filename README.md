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
| Hardware bring-up guide | WRITTEN ([docs/HARDWARE_BRINGUP.md](docs/HARDWARE_BRINGUP.md)) |
| CI: host build/tests, ASan/UBSan, PS4/PS5 SDK payload jobs | IMPLEMENTED |
| Client on Windows | UNSUPPORTED (protocol layer is portable) |
| Authentication | Architecture + shared-token mode IMPLEMENTED; disabled by default (insecure development mode) |

The PS4/PS5 builds produce payload ELFs with the official
[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) and
[ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk) SDKs, but no
console has executed them yet. Do not read "builds" as "works".

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
```

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

* 8 host unit suites via CTest (protocol, parser, env, registry, session,
  tty, shell, diagnostics).
* 24 host integration tests driving a real daemon with an independent Python
  implementation of PTTY/1, covering malformed input, authentication, session
  limits, resize, signals, multi-session isolation, timeouts, the doctor
  command (human, JSON, busy session, client binary) and a disconnect/fd-leak
  stress loop.
* `cli_test` is the controlled external-execution target (argv, environment,
  `isatty`, window size, stdin, stdout/stderr, exit code 7).

## License

GPLv3. The PS4/PS5 process backends follow the minimal concepts of the
GPLv3 [ps4/ps5-payload-dev](https://github.com/ps5-payload-dev) ELF loaders.
